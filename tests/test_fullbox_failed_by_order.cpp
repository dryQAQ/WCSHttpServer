// ============================================================================
// test_fullbox_failed_by_order.cpp — ★ 2026-09-16 现场需求④⑤（可执行自测）
//
// 需求（客户口径）：
//   ④ 「一键满箱回传」旁显示：本波次满箱回传次数 / 本次一键回传次数 / 本次一键失败次数
//      → 其中"本波次满箱回传次数"取自 outbox_fullbox 的**逐波次状态计数**
//        （getFullboxStatusCountAll：成功/待发/失败 三类，含自动满箱与手动/一键重传）；
//   ⑤ 「重传满箱切换(H7)」旁的下拉框只显示**本波次**的数据
//      → getFailedOutboxFullboxByOrder：只取该波次的 failed/cancelled 报文。
//
// 本测试锁定这条链路的数据层契约：
//   ① 按波次取失败 H7：只返回该波次的 failed/cancelled；其它波次一条都不掺入
//   ② 逐波次状态计数：success / pending(待发，含 cancelled) / failed 与逐条统计逐位一致
//   ③ 跨波次隔离：A 的计数与失败清单不受 B 的报文影响
//
// 构建：见 tests\run_tests.bat
// ============================================================================

#include <QCoreApplication>
#include <QFile>
#include <QDateTime>
#include <QDebug>
#include <cstdio>

#include "SortingDatabase.h"
#include "define.h"

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

// 造一条 H7 出站报文（msgId 自定，状态可控）
static OutboxRecord mkFullbox(const QString& msgId, const QString& orderCode,
                              const QString& grid, const QString& status)
{
    OutboxRecord m;
    m.msgId      = msgId;
    m.orderCode  = orderCode;
    m.boxcode    = "H-TEST-" + grid;
    m.grid       = grid;
    m.payload    = "{\"head\":{\"orderCode\":\"" + orderCode + "\"},\"details\":[]}";
    m.status     = status;                 // insertOutboxFullbox 会置 pending，随后用 update 改写
    m.retryCount = 0;
    m.nextRetry  = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    m.createdAt  = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    return m;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString dbPath = QCoreApplication::applicationDirPath() + "/test_fullbox_failed.db";
    QFile::remove(dbPath);
    QFile::remove(dbPath + "-wal");
    QFile::remove(dbPath + "-shm");

    SortingDatabase& db = SortingDatabase::instance();
    std::printf("== ① 打开数据库 ==\n");
    check(db.open(dbPath), QString("数据库打开成功"), dbPath);

    const QString WA = "PP2026FULLBOX_A";
    const QString WB = "PP2026FULLBOX_B";
    db.upsertReturnWave(WA, 10, 3);
    db.upsertReturnWave(WB, 10, 3);

    // ── 造数：A 波次 4 条（1 成功 / 1 待发 / 2 失败-同格口）; B 波次 2 条（1 成功 / 1 失败）──
    auto add = [&](const QString& msgId, const QString& oc, const QString& grid, const QString& finalStatus) {
        db.insertOutboxFullbox(mkFullbox(msgId, oc, grid, finalStatus));
        if (finalStatus != "pending")
            db.updateOutboxFullboxStatus(msgId, finalStatus,
                QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    };
    add("A-OK",    WA, "001", "success");
    add("A-PEND",  WA, "002", "pending");
    add("A-FAIL1", WA, "003", "failed");
    add("A-FAIL2", WA, "003", "failed");     // 同格口第二条失败 → 聚合 failCount = 2
    add("B-OK",    WB, "011", "success");
    add("B-FAIL",  WB, "012", "cancelled");  // 切出后取消重试 = 待人工重传

    std::printf("== ② 需求⑤：按波次取失败 H7（下拉只看本波次）==\n");
    {
        const QVector<OutboxRecord> a = db.getFailedOutboxFullboxByOrder(WA, 200);
        bool onlyA = true, onlyFailedOrCancelled = true;
        for (const OutboxRecord& r : a)
        {
            if (r.orderCode != WA) onlyA = false;
            if (r.status != "failed" && r.status != "cancelled") onlyFailedOrCancelled = false;
        }
        check(a.size() == 2, QString("A 波次失败报文 2 条（B 波次的 1 条不掺入）"),
              QString("count=%1").arg(a.size()));
        check(onlyA, QString("返回结果全部属于 A 波次（跨波次隔离）"));
        check(onlyFailedOrCancelled, QString("只含 failed/cancelled（success/pending 不出现）"));

        const QVector<OutboxRecord> b = db.getFailedOutboxFullboxByOrder(WB, 200);
        check(b.size() == 1 && b.first().msgId == "B-FAIL",
              QString("B 波次失败报文 1 条（cancelled 也算待人工重传）"),
              QString("count=%1 msg=%2").arg(b.size()).arg(b.isEmpty() ? "-" : b.first().msgId));

        // 波次为空 → 空表（UI 显示"本波次暂无失败记录"/"暂无运行波次"）
        check(db.getFailedOutboxFullboxByOrder("", 200).isEmpty(),
              QString("波次为空 → 返回空表（不返回全部历史）"));
        check(db.getFailedOutboxFullboxByOrder("PP2026NOT_EXIST", 200).isEmpty(),
              QString("不存在的波次 → 返回空表"));
    }

    std::printf("== ③ 需求④：逐波次 H7 状态计数（本波次满箱回传次数）==\n");
    {
        bool ok = false;
        const QMap<QString, OutboxStatusCount> all = db.getFullboxStatusCountAll(&ok);
        check(ok, QString("批量统计查询执行成功"));

        const OutboxStatusCount a = all.value(WA);
        check(a.success == 1 && a.failed == 2,
              QString("A 波次：成功 1 / 失败 2"),
              QString("succ=%1 fail=%2").arg(a.success).arg(a.failed));
        // 口径：pending 列 = "待发"（pending + cancelled + 其它），与波次列表现状一致
        check(a.pending == 1, QString("A 波次：待发 1（pending）"), QString("pend=%1").arg(a.pending));
        check(a.total() == 4, QString("A 波次满箱回传合计 4 次（= 该波次已生成的 H7 报文数）"),
              QString("total=%1").arg(a.total()));

        const OutboxStatusCount b = all.value(WB);
        check(b.success == 1 && b.pending == 1 && b.failed == 0,
              QString("B 波次：成功 1 / 待发 1（cancelled 计入待发）/ 失败 0"),
              QString("succ=%1 pend=%2 fail=%3").arg(b.success).arg(b.pending).arg(b.failed));
        check(b.total() == 2, QString("B 波次合计 2 次（与 A 互不影响）"), QString("total=%1").arg(b.total()));

        // 与"逐条统计"逐位比对（保证批量口径没有漂移）
        int succ = 0, pend = 0, fail = 0;
        for (const OutboxRecord& r : db.getOutboxFullboxByOrder(WA))
        {
            if (r.status == "success") ++succ;
            else if (r.status == "failed") ++fail;
            else ++pend;
        }
        check(succ == a.success && pend == a.pending && fail == a.failed,
              QString("批量统计与逐条统计逐位一致（succ/pend/fail）"),
              QString("逐条=%1/%2/%3 批量=%4/%5/%6").arg(succ).arg(pend).arg(fail)
                    .arg(a.success).arg(a.pending).arg(a.failed));
    }

    db.close();
    std::printf("\n===== 结果：通过 %d 项，失败 %d 项 =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
