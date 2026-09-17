// ============================================================================
// test_wave_records_batch.cpp — ★ 2026-09-15 性能修复验证（可执行自测，非产品代码）
//
// 背景（现场问题）：「波次数据记录」列表每次刷新原先是**每个波次 3 次同步 DB 查询**
//   （H7 状态 / H8 状态 / 处理件数），108 个波次 ≈ 320 次主线程往返 ≈ 数秒界面冻结，
//   点「切换波次」时最明显（切换后会立即刷新该列表）。
// 修复：改为 3 条 GROUP BY 批量查询（getFullboxStatusCountAll / getEndStatusCountAll /
//   getPendingExceptionCountAll），单次刷新与波次数量无关。
//
// 本测试用真实 SortingDatabase 造多波次/多状态数据，逐条断言：
//   ① 批量结果 == 原逐波次逻辑的统计结果（H7/H8 状态计数、处理件数）—— 口径零回归
//   ② 状态归类：success→成功、failed→失败、pending/cancelled→待发
//   ③ 未闭环过滤：仅 plc_no_grid / plc_info_incomplete 且 handled=0 计入，并按 EPC 去重
//   ④ 无记录波次：批量结果中无该键（调用方按 0 处理），逐波次查询同样为空
//   ⑤ 批量 3 次查询覆盖全部 N 个波次：耗时明显低于逐波次 N×3 次（打印实测对比）
//
// 构建：见 tests\run_tests.bat
// ============================================================================

#include <QCoreApplication>
#include <QFile>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <cstdio>

#include "SortingDatabase.h"
#include "define.h"   // SORTING_QUERY_MAX_RESULTS 等口径宏

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const QString& what, const QString& got = QString())
{
    if (ok) { ++g_pass; std::printf("  [PASS] %s\n", what.toUtf8().constData()); }
    else
    {
        ++g_fail;
        std::printf("  [FAIL] %s%s\n", what.toUtf8().constData(),
                    got.isEmpty() ? "" : ("  ← 实际: " + got).toUtf8().constData());
    }
}

static QString statusStr(const OutboxStatusCount& c)
{
    return QString("%1/%2/%3").arg(c.success).arg(c.pending).arg(c.failed);
}

// ── 改造前"逐波次"逻辑（金标准）：从列表统计 H7/H8 状态 ──
static OutboxStatusCount goldenStatusFromRows(const QVector<OutboxRecord>& rows)
{
    OutboxStatusCount c;
    for (const OutboxRecord& r : rows)
    {
        if (r.status == "success")     ++c.success;
        else if (r.status == "failed") ++c.failed;
        else                           ++c.pending;
    }
    return c;
}

// ── 改造前"逐波次"逻辑（金标准）：未闭环 PLC 判定失败的去重 EPC 数 ──
static int goldenPendingExcCount(SortingDatabase& db, const QString& orderCode)
{
    QSet<QString> epcs;
    for (const ExceptionRecord& e : db.queryExceptions(orderCode, QString(), QString(),
                                                      QString(), QString(), SORTING_QUERY_MAX_RESULTS))
    {
        if ((e.type == "plc_no_grid" || e.type == "plc_info_incomplete")
            && !e.epc.isEmpty() && !e.handled)
            epcs.insert(e.epc);
    }
    return epcs.size();
}

// ── 造数辅助：出站报文按生产口径写入（先 pending 插入，再按需改状态）──
//   注意：insertOutboxFullbox/insertOutboxEnd 只写"待发"行（status 取表默认值 pending），
//   成功/失败/取消状态是之后由 updateOutbox*Status / markOutbox*Success 更新的（与生产一致）。
static void addFullbox(SortingDatabase& db, const QString& order, const QString& status,
                       int n, const QString& tag)
{
    for (int i = 0; i < n; ++i)
    {
        OutboxRecord r;
        r.msgId = QString("H7-%1-%2-%3-%4").arg(order).arg(tag).arg(status).arg(i);
        r.orderCode = order;
        r.boxcode = "H-T0001";
        r.grid = "001";
        r.payload = "{}";
        r.nextRetry = "2026-09-15 00:00:00";     // 非 null（空值会被 NOT NULL 拒绝）
        if (!db.insertOutboxFullbox(r)) { std::printf("     [警告] H7 造数插入失败 %s\n", r.msgId.toUtf8().constData()); continue; }
        if (status == "success")        db.markOutboxFullboxSuccess(r.msgId);
        else if (status != "pending")   db.updateOutboxFullboxStatus(r.msgId, status, "2026-09-15 00:00:00");
    }
}

static void addEnd(SortingDatabase& db, const QString& order, const QString& status,
                   int n, const QString& tag)
{
    for (int i = 0; i < n; ++i)
    {
        OutboxRecord r;
        r.msgId = QString("H8-%1-%2-%3-%4").arg(order).arg(tag).arg(status).arg(i);
        r.orderCode = order;
        r.payload = "{}";
        r.nextRetry = "2026-09-15 00:00:00";
        if (!db.insertOutboxEnd(r)) { std::printf("     [警告] H8 造数插入失败 %s\n", r.msgId.toUtf8().constData()); continue; }
        if (status == "success")       db.markOutboxEndSuccess(r.msgId);
        else if (status != "pending")  db.updateOutboxEndStatus(r.msgId, status, "2026-09-15 00:00:00");
    }
}

static void addException(SortingDatabase& db, const QString& order, const QString& type, const QString& epc)
{
    ExceptionRecord e;
    e.type = type;
    e.orderCode = order;
    e.epc = epc;
    e.sku = "SKU1";
    e.reason = "自测造数";
    e.time = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    db.insertException(e);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString dbPath = QCoreApplication::applicationDirPath() + "/test_wave_records.db";
    QFile::remove(dbPath);
    QFile::remove(dbPath + "-wal");
    QFile::remove(dbPath + "-shm");

    SortingDatabase& db = SortingDatabase::instance();
    std::printf("== ① 打开数据库（自动建表）==\n");
    check(db.open(dbPath), QString("数据库打开成功"), dbPath);

    // ── 造数：4 个波次（分拣中/已完结/已取消/无记录）──
    const QString W1 = "TEST-WAVE-SORTING";     // 分拣中：H7 三种状态都有；H8 待发；异常多条含重复/已闭环/类型不符
    const QString W2 = "TEST-WAVE-FINISHED";    // 已完结：H7 成功；H8 成功；异常 1 条
    const QString W3 = "TEST-WAVE-CANCELLED";   // 已取消：H7 cancelled（算"待发"）；H8 failed；无异常
    const QString W4 = "TEST-WAVE-EMPTY";       // 无任何报文/异常

    db.upsertReturnWave(W1, 10, 3);
    db.upsertReturnWave(W2, 10, 8);
    db.upsertReturnWave(W3, 10, 6);
    db.upsertReturnWave(W4, 10, 1);

    addFullbox(db, W1, "success", 1, "a");
    addFullbox(db, W1, "pending", 1, "a");
    addFullbox(db, W1, "failed",  1, "a");
    addFullbox(db, W2, "success", 2, "a");
    addFullbox(db, W3, "cancelled", 3, "a");   // cancelled 归"待发"（与原逐波次口径一致）

    addEnd(db, W1, "pending", 2, "a");
    addEnd(db, W2, "success", 1, "a");
    addEnd(db, W3, "failed",  1, "a");

    addException(db, W1, "plc_no_grid", "EPC-A");            // 计入
    addException(db, W1, "plc_no_grid", "EPC-A");            // 同 EPC 重复 → 去重仍算 1
    addException(db, W1, "plc_info_incomplete", "EPC-B");    // 计入
    addException(db, W1, "plc_no_grid", "EPC-C");            // 计入后闭环 → 不计
    addException(db, W1, "conflict", "EPC-D");               // 类型不符 → 不计
    addException(db, W2, "plc_no_grid", "EPC-E");            // 计入
    db.markExceptionResolved(W1, "EPC-C");                   // 闭环 EPC-C

    std::printf("== ② 批量查询结果（3 次查询覆盖全部波次）==\n");
    bool okH7 = false, okH8 = false, okExc = false;
    const QMap<QString, OutboxStatusCount> h7 = db.getFullboxStatusCountAll(&okH7);
    const QMap<QString, OutboxStatusCount> h8 = db.getEndStatusCountAll(&okH8);
    const QMap<QString, int>               exc = db.getPendingExceptionCountAll(&okExc);

    check(okH7 && okH8 && okExc, QString("三条批量查询均执行成功"),
          QString("%1/%2/%3").arg(okH7).arg(okH8).arg(okExc));

    check(statusStr(h7.value(W1)) == "1/1/1", QString("H7 W1 成功1/待发1/失败1"), statusStr(h7.value(W1)));
    check(statusStr(h7.value(W2)) == "2/0/0", QString("H7 W2 成功2/待发0/失败0"), statusStr(h7.value(W2)));
    check(statusStr(h7.value(W3)) == "0/3/0", QString("H7 W3 cancelled 归『待发』→ 0/3/0"), statusStr(h7.value(W3)));
    check(!h7.contains(W4), QString("H7 W4（无报文）批量结果中无该键 → 显示『无』"));

    check(statusStr(h8.value(W1)) == "0/2/0", QString("H8 W1 成功0/待发2/失败0"), statusStr(h8.value(W1)));
    check(statusStr(h8.value(W2)) == "1/0/0", QString("H8 W2 成功1/待发0/失败0"), statusStr(h8.value(W2)));
    check(statusStr(h8.value(W3)) == "0/0/1", QString("H8 W3 成功0/待发0/失败1"), statusStr(h8.value(W3)));

    check(exc.value(W1, -1) == 2, QString("处理件数 W1 = 2（EPC-A 去重 + EPC-B；EPC-C 已闭环、EPC-D 类型不符不计）"),
          QString::number(exc.value(W1, -1)));
    check(exc.value(W2, -1) == 1, QString("处理件数 W2 = 1"), QString::number(exc.value(W2, -1)));
    check(!exc.contains(W3), QString("处理件数 W3（无异常）无该键 → 按 0 处理"));

    std::printf("== ③ 与改造前『逐波次』逻辑逐条比对（口径零回归）==\n");
    {
        const QStringList allWaves = {W1, W2, W3, W4};
        bool h7Same = true, h8Same = true, excSame = true;
        for (const QString& w : allWaves)
        {
            const OutboxStatusCount g7 = goldenStatusFromRows(db.getOutboxFullboxByOrder(w));
            const OutboxStatusCount g8 = goldenStatusFromRows(db.getOutboxEndByOrder(w));
            const OutboxStatusCount b7 = h7.value(w);        // 无键 = 全 0（与"无报文"一致）
            const OutboxStatusCount b8 = h8.value(w);

            if (statusStr(b7) != statusStr(g7)) { h7Same = false; std::printf("     H7 不一致 wave=%s 批量=%s 逐波次=%s\n", w.toUtf8().constData(), statusStr(b7).toUtf8().constData(), statusStr(g7).toUtf8().constData()); }
            if (statusStr(b8) != statusStr(g8)) { h8Same = false; std::printf("     H8 不一致 wave=%s 批量=%s 逐波次=%s\n", w.toUtf8().constData(), statusStr(b8).toUtf8().constData(), statusStr(g8).toUtf8().constData()); }

            const int gExc = goldenPendingExcCount(db, w);
            const int bExc = exc.value(w, 0);
            if (gExc != bExc) { excSame = false; std::printf("     处理件数不一致 wave=%s 批量=%d 逐波次=%d\n", w.toUtf8().constData(), bExc, gExc); }
        }
        check(h7Same, QString("H7 状态计数：批量 == 逐波次（4 个波次全部一致）"));
        check(h8Same, QString("H8 状态计数：批量 == 逐波次"));
        check(excSame, QString("处理件数：批量 == 逐波次"));
    }

    std::printf("== ④ 规模对比：N 个波次时批量查询与逐波次查询耗时 ==\n");
    {
        const int N = 60;
        QVector<QString> orders;
        orders.reserve(N);
        for (int i = 0; i < N; ++i)
        {
            const QString o = QString("PERF-WAVE-%1").arg(i, 3, 10, QChar('0'));
            orders.append(o);
            db.upsertReturnWave(o, 5, 3);
            addFullbox(db, o, (i % 3 == 0) ? "success" : (i % 3 == 1 ? "pending" : "failed"), 1, "p");
            addEnd(db, o, "success", 1, "p");
            addException(db, o, "plc_no_grid", QString("EPC-P%1").arg(i));
            addException(db, o, "plc_no_grid", QString("EPC-P%1").arg(i));   // 重复，验证去重
        }

        QElapsedTimer t1; t1.start();
        bool ok1 = false, ok2 = false, ok3 = false;
        const QMap<QString, OutboxStatusCount> b7 = db.getFullboxStatusCountAll(&ok1);
        const QMap<QString, OutboxStatusCount> b8 = db.getEndStatusCountAll(&ok2);
        const QMap<QString, int>               be = db.getPendingExceptionCountAll(&ok3);
        const qint64 batchMs = t1.elapsed();

        QElapsedTimer t2; t2.start();
        int goldenSum = 0;
        for (const QString& o : orders)
        {
            goldenSum += goldenStatusFromRows(db.getOutboxFullboxByOrder(o)).total();
            goldenSum += goldenStatusFromRows(db.getOutboxEndByOrder(o)).total();
            goldenSum += goldenPendingExcCount(db, o);
        }
        const qint64 perWaveMs = t2.elapsed();

        std::printf("     N=%d 波次：批量 3 次查询 %.1f ms ／ 逐波次 %d 次查询 %.1f ms（金标准合计校验值=%d）\n",
                    N, (double)batchMs, N * 3, (double)perWaveMs, goldenSum);
        check(ok1 && ok2 && ok3, QString("N 个波次时批量查询仍全部成功"));
        check(b7.value(orders.first()).total() == 1 && b8.value(orders.first()).total() == 1
                  && be.value(orders.first()) == 1,
              QString("N 个波次时首波次统计正确（H7=1 H8=1 处理=1）"),
              QString("%1/%2/%3").arg(b7.value(orders.first()).total())
                                 .arg(b8.value(orders.first()).total()).arg(be.value(orders.first())));
        check(batchMs < perWaveMs, QString("批量查询耗时 < 逐波次查询耗时（与波次数无关）"),
              QString("批量 %1ms vs 逐波次 %2ms").arg(batchMs).arg(perWaveMs));
    }

    db.close();
    std::printf("\n===== 结果：通过 %d 项，失败 %d 项 =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
