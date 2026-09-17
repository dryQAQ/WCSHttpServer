// ============================================================================
// test_sorting_query_date.cpp — ★ 2026-09-16 现场需求③（可执行自测，非产品代码）
//
// 需求（客户口径）：
//   「分拣记录查询：结合日期条件筛选」——日期区间**必须生效**（界面没有"全部/不筛日期"选项），
//   四种查询模式（按EPC / 按SKU / 按格口 / 按容器号）的结果都只包含区间内的落格记录；
//   「待分拣」计划行（return_wave_item）没有落格时间，**不受日期筛选影响**。
//
// 本测试在数据库层锁定这条口径（驱动真实 SortingDatabase，不 mock SQL）：
//   ① 五个查询函数在 [D, D] 区间下只返回 D 当日落格；区间外记录一条都不出现
//   ② 边界包含性：D 00:00:00 与 D 23:59:59 必须命中；D-1 23:59:59 与 D+1 00:00:00 必须排除
//   ③ 待分拣（计划）行不受日期影响（同 SKU 计划在区间外仍在结果里）
//   ④ queryGridSummary(from,to)：计数 / SKU数 / 最近分拣时间都只反映区间内
//   ⑤ queryOtherBoxcodesByEpc：跨容器反查与主查询同区间（区间外的"别的箱"不返回）
//
// 构建：见 tests\run_tests.bat
// ============================================================================

#include <QCoreApplication>
#include <QFile>
#include <QDateTime>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
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

// 测试专用：把**当前这批**刚落库的明细时间写成指定时刻。
//   为什么不能用产品接口做到：insertRecord 的 sort_time 固定取"当前时间"
//   （落格时间的唯一来源就是 PLC 反馈那一刻），本用例需要跨 3 天的可判定数据。
//   实现（两步，确定性、不误伤历史行）：
//     ① 先把"今天"的所有行统一改成哨兵时间 2000-01-01（刚落库的行时间必然带今天日期）；
//     ② 再把哨兵行按 EPC 逐条写成目标时刻 —— 因此每条只被改写一次，不存在"把别的行也改掉"
//        （曾用"按 EPC + 日期前缀"定位，结果同一 EPC 的旧行也被连带改写，已弃用）。
static const char* kSentinelDay = "2000-01-01";

static int sentinelizeTodayRows(const QString& dbPath, const QString& todayPrefix,
                                const QStringList& keepTimes)
{
    int changed = 0;
    {
        QSqlDatabase c = QSqlDatabase::addDatabase("QSQLITE", "datefix0");
        c.setDatabaseName(dbPath);
        if (c.open())
        {
            QSqlQuery pragma(c);
            pragma.exec("PRAGMA busy_timeout = 5000");
            QSqlQuery q(c);
            // ★ 必须排除"已经写成目标时刻"的行：本用例有目标时刻恰好落在今天（D 当日），
            //   否则下面按 EPC 精确改写时，那批行会被再次纳入哨兵区而丢失目标时刻。
            //   （不要用 LIKE '%-00:00:00' 这类模式：`-` 是普通字符，会连带排除掉目标时刻本身。）
            QString sql = "UPDATE sorting_records SET sort_time = ? "
                          "WHERE substr(sort_time, 1, 10) = ?";
            for (int i = 0; i < keepTimes.size(); ++i)
                sql += QString(" AND sort_time <> ?%1").arg(i + 1);
            q.prepare(sql);
            q.addBindValue(QString("%1 00:00:00").arg(kSentinelDay));
            q.addBindValue(todayPrefix);
            for (const QString& t : keepTimes) q.addBindValue(t);
            if (q.exec()) changed = q.numRowsAffected();
            else std::printf("  [WARN] sentinelize 失败 err=%s\n",
                             q.lastError().text().toUtf8().constData());
        }
        c.close();
    }
    QSqlDatabase::removeDatabase("datefix0");
    return changed;
}

static bool setSortTimeByEpc(const QString& dbPath, const QString& epc, const QString& timeStr)
{
    bool ok = false;
    {
        QSqlDatabase c = QSqlDatabase::addDatabase("QSQLITE", "datefix1");
        c.setDatabaseName(dbPath);
        if (c.open())
        {
            QSqlQuery pragma(c);
            pragma.exec("PRAGMA busy_timeout = 5000");
            QSqlQuery q(c);
            q.prepare("UPDATE sorting_records SET sort_time = ? "
                      "WHERE barcode = ? AND substr(sort_time, 1, 10) = ?");
            q.addBindValue(timeStr);
            q.addBindValue(epc);
            q.addBindValue(kSentinelDay);
            if (q.exec()) ok = (q.numRowsAffected() > 0);
            if (!ok)
                std::printf("  [WARN] setSortTimeByEpc 失败 epc=%s rows=%d err=%s\n",
                            epc.toUtf8().constData(), q.numRowsAffected(),
                            q.lastError().text().toUtf8().constData());
        }
        c.close();
    }
    QSqlDatabase::removeDatabase("datefix1");
    return ok;
}

// 统计结果里"某一天"的记录条数（按 sortTime 前缀判断）
static int countByDay(const QVector<SortingRecord>& recs, const QString& yyyyMMdd)
{
    int n = 0;
    for (const SortingRecord& r : recs)
        if (r.sortTime.startsWith(yyyyMMdd)) ++n;
    return n;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString dbPath = QCoreApplication::applicationDirPath() + "/test_sorting_query_date.db";
    QFile::remove(dbPath);
    QFile::remove(dbPath + "-wal");
    QFile::remove(dbPath + "-shm");

    SortingDatabase& db = SortingDatabase::instance();
    std::printf("== ① 打开数据库 ==\n");
    check(db.open(dbPath), QString("数据库打开成功"), dbPath);

    // ── 造数：D-1 / D / D+1 三天的落格明细 + 一条"无落格"的计划 ──
    //   D 当日刻意放两条边界记录（00:00:00 与 23:59:59），用于验证区间"含首含尾"。
    const QDate D = QDate(2026, 9, 16);
    const QString dPrev = D.addDays(-1).toString("yyyy-MM-dd");
    const QString dCur  = D.toString("yyyy-MM-dd");
    const QString dNext = D.addDays(1).toString("yyyy-MM-dd");

    const QString WAVE  = "PP2026DATEFILTER";
    const QString BOXA  = "H-DATE-A";     // 区间内落在本容器
    const QString BOXB  = "H-DATE-B";     // 区间外（D-1）落过同一 EPC → 跨容器反查用
    db.upsertReturnWave(WAVE, 10, 3 /*WAVE_SORTING*/);

    // 本用例用到的全部目标时刻：哨兵化时必须排除它们，避免把已写好的时间又纳回哨兵区
    const QStringList kTargetTimes = QStringList()
        << dPrev + " 23:59:59"
        << dPrev + " 23:00:00"
        << dCur  + " 00:00:00"
        << dCur  + " 12:00:00"
        << dCur  + " 23:59:59"
        << dNext + " 00:00:00";

    auto addRec = [&](const QString& epc, const QString& sku, const QString& grid,
                      const QString& box, const QString& timeStr) {
        db.insertRecord(WAVE, epc, sku, grid, "1", "1", "2", 1, "H-01-AB", box);
        // 刚落库的行时间带"今天"前缀 → 先纳入哨兵区，再按 EPC 精确写成目标时刻
        sentinelizeTodayRows(dbPath, QDate::currentDate().toString("yyyy-MM-dd"), kTargetTimes);
        if (!setSortTimeByEpc(dbPath, epc, timeStr))
            std::printf("  [WARN] 时间改写失败 epc=%s -> %s\n",
                        epc.toUtf8().constData(), timeStr.toUtf8().constData());
    };

    // D-1：EPC-0（同格口 001、不同容器 BOXB）—— 用于验证"区间外不出现"与跨容器反查
    addRec("EPC-0",   "SKU-D0", "001", BOXB, dPrev + " 23:59:59");
    // EPC-1 在 D-1 也曾落到 BOXB（重扫重投 + 中途换箱的典型跨容器场景）
    //   → 用于验证"跨容器反查与主查询同区间"：只查 D 当天时不报跨容器，放宽到 D-1~D 才报
    addRec("EPC-1",   "SKU-D1", "001", BOXB, dPrev + " 23:00:00");
    // D 当日：3 条（含两条边界）
    addRec("EPC-1",   "SKU-D1", "001", BOXA, dCur  + " 00:00:00");   // 边界：起
    addRec("EPC-2",   "SKU-D2", "002", BOXA, dCur  + " 12:00:00");
    addRec("EPC-3",   "SKU-D2", "002", BOXA, dCur  + " 23:59:59");   // 边界：止
    // D+1：区间外
    addRec("EPC-4",   "SKU-D3", "003", BOXA, dNext + " 00:00:00");

    // 计划明细：SKU-D9 有计划但**从未落格** → 必须始终出现在结果里（不受日期影响）
    {
        QVector<ReturnWaveItemRecord> items;
        ReturnWaveItemRecord it;
        it.orderCode = WAVE; it.inco = "SKU-D9"; it.gridNum = "009"; it.gridType = "0";
        it.planQty = 4; it.volu = "H-01-ZZ"; it.obxCode = BOXA;
        items.append(it);
        db.insertWaveItems(WAVE, items);
    }

    const QDateTime from(D, QTime(0, 0, 0));
    const QDateTime to(D, QTime(23, 59, 59));

    std::printf("== ② 按EPC查询（含留空查全部）只返回区间内 ==\n");
    {
        const QVector<SortingRecord> r1 = db.queryByBarcode("EPC-1", from, to, SORTING_QUERY_MAX_RESULTS);
        check(r1.size() == 1 && countByDay(r1, dCur) == 1,
              QString("按EPC [EPC-1]：只返回 D 当日 1 条（D-1 的同 EPC 记录被排除）"),
              QString("D=%1 总=%2").arg(countByDay(r1, dCur)).arg(r1.size()));

        const QVector<SortingRecord> all = db.queryAllWithPending(from, to, SORTING_QUERY_MAX_RESULTS);
        // 已分拣部分：应为 D 当日 3 条（EPC-1/2/3）；待分拣 1 条（SKU-D9）不受日期影响
        int sortedCnt = 0, pendingCnt = 0;
        for (const SortingRecord& r : all)
            (r.status == QString::fromUtf8("待分拣") ? pendingCnt : sortedCnt)++;
        check(sortedCnt == 3, QString("留空查全部：已分拣只返回 D 当日 3 条（D-1 两条 + D+1 一条被排除）"),
              QString("已分拣=%1 总=%2").arg(sortedCnt).arg(all.size()));
        check(pendingCnt == 1, QString("「待分拣」计划行不受日期影响，照常返回 1 条"),
              QString("待分拣=%1").arg(pendingCnt));
    }

    std::printf("== ③ 边界包含性：00:00:00 与 23:59:59 必须命中 ==\n");
    {
        const QVector<SortingRecord> rb = db.queryByBarcode("EPC-2", from, to, SORTING_QUERY_MAX_RESULTS);
        check(countByDay(rb, dCur) == 1, QString("D 12:00:00 的记录命中"), QString::number(rb.size()));

        const QVector<SortingRecord> rStart = db.queryByBarcode("EPC-1", from, to, SORTING_QUERY_MAX_RESULTS);
        check(rStart.size() == 1 && countByDay(rStart, dCur) == 1,
              QString("D 00:00:00（区间起点）命中 —— 含首"),
              QString("size=%1").arg(rStart.size()));

        const QVector<SortingRecord> rEnd = db.queryByBarcode("EPC-3", from, to, SORTING_QUERY_MAX_RESULTS);
        check(countByDay(rEnd, dCur) == 1, QString("D 23:59:59（区间终点）命中 —— 含尾"),
              QString::number(countByDay(rEnd, dCur)));

        const QVector<SortingRecord> rOut = db.queryByBarcode("EPC-4", from, to, SORTING_QUERY_MAX_RESULTS);
        check(countByDay(rOut, dCur) == 0 && countByDay(rOut, dNext) == 0,
              QString("D+1 00:00:00 的记录**不**命中（区间外）"),
              QString("hits=%1").arg(rOut.size()));
    }

    std::printf("== ④ 按格口 / 按SKU / 按容器号 同口径 ==\n");
    {
        const QVector<SortingRecord> g = db.queryByGrid("001", from, to, SORTING_QUERY_MAX_RESULTS);
        check(g.size() == 1 && g.first().barcode == "EPC-1",
              QString("按格口 [001]：只返回 D 当日 1 条（D-1 的 EPC-0 被排除）"),
              QString("count=%1").arg(g.size()));

        const QVector<SortingRecord> s = db.queryBySku("SKU-D2", from, to, SORTING_QUERY_MAX_RESULTS);
        check(s.size() == 2, QString("按SKU [SKU-D2]：只返回 D 当日 2 条（EPC-2/EPC-3）"),
              QString("count=%1").arg(s.size()));

        const QVector<SortingRecord> b = db.queryByBoxcode(BOXA, from, to, SORTING_QUERY_MAX_RESULTS);
        check(b.size() == 3, QString("按容器号 [%1]：只返回 D 当日 3 条（D+1 那条被排除）").arg(BOXA),
              QString("count=%1").arg(b.size()));

        // 空区间（区间内无数据）→ 0 条
        const QDateTime f2(D.addDays(-30), QTime(0, 0, 0));
        const QDateTime t2(D.addDays(-30), QTime(23, 59, 59));
        check(db.queryByGrid("001", f2, t2, SORTING_QUERY_MAX_RESULTS).isEmpty(),
              QString("区间内无数据 → 返回 0 条（不自动放宽到全量）"));
    }

    std::printf("== ⑤ 全格口汇总：计数/最近时间只反映区间内 ==\n");
    {
        const QVector<GridSummaryRecord> sums = db.queryGridSummary(from, to);
        int total = 0;
        QString grid001Time, grid001Box;
        for (const GridSummaryRecord& g : sums)
        {
            total += g.sortedCount;
            if (g.gridNum == "001") { grid001Time = g.lastSortTime; grid001Box = g.boxcode; }
        }
        check(total == 3, QString("汇总合计 = D 当日 3 件（不含 D-1/D+1）"), QString::number(total));
        check(grid001Time.startsWith(dCur),
              QString("格口001 最近分拣时间落在 D 当日（%1）").arg(grid001Time), grid001Time);
        check(grid001Box == BOXA,
              QString("格口001 最近容器号取区间内的 %1（不是区间外的 %2）").arg(BOXA).arg(BOXB), grid001Box);
    }

    std::printf("== ⑥ 跨容器反查（同EPC其他容器）与主查询同区间 ==\n");
    {
        // EPC-1 在 D-1 落到 BOXB、在 D 落到 BOXA。
        //   口径（有意为之）：反查与主查询**同区间** —— 只查 D 当天时，D-1 的 BOXB 不参与判断，
        //   因此不会报"跨容器"。这样"结果被日期裁剪、反查却按全量"的口径打架不会出现；
        //   需要做跨天漂移核对时，把起始日期往前放宽即可（下一条断言即证明它按区间生效）。
        const QMap<QString, QStringList> other =
            db.queryOtherBoxcodesByEpc(QStringList() << "EPC-1", BOXA, from, to);
        check(!other.contains("EPC-1"),
              QString("区间 [D,D] 内该 EPC 在本容器的记录之外没有其他容器 → 不报跨容器（区间外的 BOXB 不出现）"),
              other.contains("EPC-1") ? other.value("EPC-1").join(",") : QString("(无)"));

        // 扩大到 D-1~D → 此时应能看到 BOXB（证明过滤确实按区间生效，而不是恒不过滤）
        const QDateTime f2(D.addDays(-1), QTime(0, 0, 0));
        const QMap<QString, QStringList> other2 =
            db.queryOtherBoxcodesByEpc(QStringList() << "EPC-1", BOXA, f2, to);
        check(other2.contains("EPC-1") && other2.value("EPC-1").contains(BOXB),
              QString("扩大到 D-1~D 区间 → 能查到其他容器 %1（过滤按区间生效）").arg(BOXB),
              other2.contains("EPC-1") ? other2.value("EPC-1").join(",") : QString("(无)"));
    }

    db.close();
    std::printf("\n===== 结果：通过 %d 项，失败 %d 项 =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
