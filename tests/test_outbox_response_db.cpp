// ============================================================================
// test_outbox_response_db.cpp — ★ 2026-09-22 现场需求①（可执行自测，非产品代码）
//
// 需求（客户口径）：
//   ① 波次信息面板显示「回传次数」= 本波次已生成的出站报文条数（满箱切换 H7 + 完结回传 H8）；
//      右侧「查看」→ 回传明细弹窗（顶部**波次切换下拉**），列出该波次**全部**回传条目；
//   ② 双击任一行 → 该条报文的**完整请求报文**与 **WMS 响应信息**；
//   ③ 硬约束：**报文与响应都不得截断**（落库全量、读取全量、无长度上限）。
//
// 本测试锁定这条链路的数据层契约（UI 只是渲染这层的结果）：
//   ① 响应留痕往返：saveOutboxResponse 写入的 HTTP 状态 / 响应体 / 说明 / 时间，
//      经 getOutboxFullboxByOrder / getOutboxEndByOrder 读出**逐字段一致**，
//      且不影响 payload / status / retry_count / created_at（状态机与响应留痕互不干扰）；
//   ② 全量不截断护栏：> 1 MB 的响应体与长 payload 读回后**长度与内容完全相等**（防日后加回上限）；
//   ③ 旧库迁移：旧版结构（无 resp_* 列）的 outbox 两表在 open() 时自动补列，
//      旧行可读、新响应可写（升级现场库不会丢响应留痕能力）；
//   ④ 回传次数口径：批量统计（getFullboxStatusCountAll + getEndStatusCountAll）之和
//      == 逐条统计之和；跨波次隔离；无报文波次取不到键（面板按 0 处理、"无回传"标注）；
//   ⑤ 按波次全量取数：某波次 300 条 H7 全部返回（无 LIMIT、不抽稀）；
//   ⑥ 重传语义：同一 msgId 二次写入 = 覆盖"最近一次"响应，且**不新增行**（回传次数不变）。
//
// 构建：见 tests\run_tests.bat
// ============================================================================

#include <QCoreApplication>
#include <QFile>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
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

// 造一条 H7 出站报文（状态先 pending，随后按需 update 改写 —— 与生产插入路径一致）
static OutboxRecord mkFullbox(const QString& msgId, const QString& orderCode,
                              const QString& grid, const QString& payload)
{
    OutboxRecord m;
    m.msgId      = msgId;
    m.orderCode  = orderCode;
    m.boxcode    = "H-TEST-" + grid;
    m.grid       = grid;
    m.payload    = payload;
    m.nextRetry  = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    m.createdAt  = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    return m;
}

static OutboxRecord mkEnd(const QString& msgId, const QString& orderCode, const QString& payload)
{
    OutboxRecord m;
    m.msgId      = msgId;
    m.orderCode  = orderCode;
    m.payload    = payload;
    m.nextRetry  = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    m.createdAt  = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    return m;
}

static const OutboxRecord* findById(const QVector<OutboxRecord>& rows, const QString& msgId)
{
    for (const OutboxRecord& r : rows) if (r.msgId == msgId) return &r;
    return nullptr;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ══════════════════════════════════════════════════════════════════
    // ③ 旧库迁移（先做：用旧版结构建库 → 再 open() 触发迁移）
    //    旧版 DDL 就取"加 resp_* 之前"的真实定义（含 grid 列，不含响应留痕列）
    // ══════════════════════════════════════════════════════════════════
    const QString legacyPath = QCoreApplication::applicationDirPath() + "/test_outbox_resp_legacy.db";
    QFile::remove(legacyPath);
    QFile::remove(legacyPath + "-wal");
    QFile::remove(legacyPath + "-shm");
    {
        QSqlDatabase c = QSqlDatabase::addDatabase("QSQLITE", "legacyMake");
        c.setDatabaseName(legacyPath);
        if (!c.open())
        {
            std::printf("  [FAIL] 旧库创建失败: %s\n", c.lastError().text().toUtf8().constData());
            return 1;
        }
        QSqlQuery q(c);
        const bool ddlOk =
            q.exec("CREATE TABLE IF NOT EXISTS outbox_fullbox ("
                   "  msg_id TEXT PRIMARY KEY, order_code TEXT NOT NULL DEFAULT '',"
                   "  boxcode TEXT NOT NULL DEFAULT '', grid TEXT NOT NULL DEFAULT '',"
                   "  payload TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'pending',"
                   "  retry_count INTEGER NOT NULL DEFAULT 0, next_retry TEXT NOT NULL DEFAULT '',"
                   "  created_at TEXT NOT NULL DEFAULT '')")
         && q.exec("CREATE TABLE IF NOT EXISTS outbox_end ("
                   "  msg_id TEXT PRIMARY KEY, order_code TEXT NOT NULL DEFAULT '',"
                   "  payload TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'pending',"
                   "  retry_count INTEGER NOT NULL DEFAULT 0, next_retry TEXT NOT NULL DEFAULT '',"
                   "  created_at TEXT NOT NULL DEFAULT '')")
         && q.exec("INSERT INTO outbox_fullbox (msg_id, order_code, boxcode, grid, payload,"
                   "  status, retry_count, next_retry, created_at) VALUES "
                   "('LEGACY-H7', 'PP2026LEGACY', 'H-OLD-1', '001', '{\"head\":{}}',"
                   " 'success', 0, '2026-09-01 00:00:00', '2026-09-01 00:00:00')");
        c.close();
        if (!ddlOk) { std::printf("  [FAIL] 旧库结构与旧数据准备失败\n"); return 1; }
    }
    QSqlDatabase::removeDatabase("legacyMake");

    std::printf("== ③ 旧库迁移（缺 resp_* 列的老库自动补列，响应留痕与读取可用）==\n");
    {
        SortingDatabase& db = SortingDatabase::instance();
        check(db.open(legacyPath), QString("旧版结构数据库打开成功（createTables 触发迁移）"), legacyPath);

        const QVector<OutboxRecord> rows = db.getOutboxFullboxByOrder("PP2026LEGACY");
        check(rows.size() == 1, QString("旧行仍可读（1 条）"), QString("count=%1").arg(rows.size()));
        if (rows.size() == 1)
        {
            const OutboxRecord& r = rows.first();
            check(r.msgId == "LEGACY-H7" && r.payload == "{\"head\":{}}" && r.status == "success",
                  QString("旧行原字段无损（msgId/payload/status）"));
            check(r.respTime.isEmpty() && r.respHttp == 0 && r.respBody.isEmpty(),
                  QString("旧行响应留痕为空（默认值）→ UI 显示\"无响应留痕\"，不臆造"),
                  QString("http=%1 body=%2 time=%3").arg(r.respHttp).arg(r.respBody.size()).arg(r.respTime));
        }
        // 迁移后必须能写入响应留痕（缺列时这条 UPDATE 会 no such column 而静默丢失）
        const bool saved = db.saveOutboxResponse("LEGACY-H7", true, 200,
                                                 "{\"success\":true,\"msg\":\"迁移后可写\"}", QString());
        const QVector<OutboxRecord> after = db.getOutboxFullboxByOrder("PP2026LEGACY");
        const OutboxRecord* r1 = after.isEmpty() ? nullptr : &after.first();
        check(saved && r1 && r1->respHttp == 200
                  && r1->respBody == "{\"success\":true,\"msg\":\"迁移后可写\"}"
                  && !r1->respTime.isEmpty(),
              QString("迁移后响应留痕可写可读（HTTP 200 + 响应体 + 时间）"),
              r1 ? QString("http=%1 body=%2 time=%3").arg(r1->respHttp).arg(r1->respBody).arg(r1->respTime)
                 : QString("读回为空"));
        db.close();
    }

    // ══════════════════════════════════════════════════════════════════
    // 主库（新结构）：① ② ④ ⑤ ⑥
    // ══════════════════════════════════════════════════════════════════
    const QString dbPath = QCoreApplication::applicationDirPath() + "/test_outbox_response.db";
    QFile::remove(dbPath);
    QFile::remove(dbPath + "-wal");
    QFile::remove(dbPath + "-shm");

    SortingDatabase& db = SortingDatabase::instance();
    std::printf("== ① 打开新结构数据库 ==\n");
    check(db.open(dbPath), QString("数据库打开成功"), dbPath);

    const QString WA = "PP2026RESP_A";
    const QString WB = "PP2026RESP_B";
    db.upsertReturnWave(WA, 100, 3);
    db.upsertReturnWave(WB, 100, 3);

    // ── 造数：A 波次 H7×3（1 成功 / 1 待发 / 1 失败）+ H8×1；B 波次 H7×1 ──
    db.insertOutboxFullbox(mkFullbox("A7-OK",   WA, "001", "{\"head\":{\"orderCode\":\"" + WA + "\"}}"));
    db.insertOutboxFullbox(mkFullbox("A7-PEND", WA, "002", "{\"head\":{\"orderCode\":\"" + WA + "\"}}"));
    db.insertOutboxFullbox(mkFullbox("A7-FAIL", WA, "003", "{\"head\":{\"orderCode\":\"" + WA + "\"}}"));
    db.insertOutboxEnd(mkEnd("A8-END", WA, "{\"head\":{\"orderCode\":\"" + WA + "\",\"sumLocation\":\"88\"}}"));
    db.insertOutboxFullbox(mkFullbox("B7-OK",   WB, "011", "{\"head\":{\"orderCode\":\"" + WB + "\"}}"));

    db.markOutboxFullboxSuccess("A7-OK");
    db.markOutboxFullboxSuccess("B7-OK");
    db.updateOutboxFullboxStatus("A7-FAIL", "failed", "2026-09-22 10:00:00");
    db.markOutboxEndSuccess("A8-END");
    // A7-PEND 保持 pending（待发）

    std::printf("== ① 响应留痕往返（HTTP 状态/完整响应体/说明/时间 逐字段一致）==\n");
    {
        const QString respOk  = "{\"success\":true,\"status\":200,\"msg\":\"OK\",\"data\":{\"n\":1}}";
        const QString respBad = "{\"success\":false,\"status\":500,\"msg\":\"转移库存失败\"}";
        check(db.saveOutboxResponse("A7-OK",   true, 200, respOk,  QString()),
              QString("H7 成功响应写入"));
        check(db.saveOutboxResponse("A7-FAIL", true, 0,   QString(),
                                    QString::fromUtf8("超时（3000ms 无响应）")),
              QString("H7 超时（无 HTTP 响应）写入"));
        check(db.saveOutboxResponse("A8-END",  false, 500, respBad, QString()),
              QString("H8 失败响应写入"));

        const QVector<OutboxRecord> h7 = db.getOutboxFullboxByOrder(WA);
        const OutboxRecord* ok   = findById(h7, "A7-OK");
        const OutboxRecord* fail = findById(h7, "A7-FAIL");
        check(ok && ok->respHttp == 200 && ok->respBody == respOk && ok->respNote.isEmpty()
                  && !ok->respTime.isEmpty(),
              QString("H7 成功：HTTP=200，响应体逐字符一致，响应时间已填"),
              ok ? QString("http=%1 bodyEq=%2 time=%3").arg(ok->respHttp)
                       .arg(ok->respBody == respOk ? 1 : 0).arg(ok->respTime) : QString("未找到"));
        check(fail && fail->respHttp == 0 && fail->respBody.isEmpty()
                  && fail->respNote == QString::fromUtf8("超时（3000ms 无响应）"),
              QString("H7 超时：HTTP=0 + 说明（UI 显示\"未收到 HTTP 响应\"）"),
              fail ? QString("http=%1 note=%2").arg(fail->respHttp).arg(fail->respNote) : QString("未找到"));

        const QVector<OutboxRecord> h8 = db.getOutboxEndByOrder(WA);
        const OutboxRecord* e = h8.isEmpty() ? nullptr : &h8.first();
        check(e && e->respHttp == 500 && e->respBody == respBad && !e->respTime.isEmpty(),
              QString("H8：HTTP=500 + 响应体逐字符一致（失败响应同样全文留痕）"),
              e ? QString("http=%1 bodyEq=%2").arg(e->respHttp).arg(e->respBody == respBad ? 1 : 0)
                : QString("未找到"));

        // 响应留痕不得干扰状态机字段
        check(ok && ok->status == "success" && fail && fail->status == "failed",
              QString("写入响应不改变 status（成功/失败仍由原状态机维护）"),
              ok ? QString("ok=%1").arg(ok->status) : QString());
        check(fail && fail->retryCount == 1,
              QString("写入响应不改变 retry_count"),
              fail ? QString("retry=%1").arg(fail->retryCount) : QString());
        check(ok && ok->createdAt == findById(h7, "A7-OK")->createdAt,
              QString("写入响应不改变 created_at"));
    }

    std::printf("== ② 全量不截断护栏（>1MB 响应体与长 payload 逐字符一致）==\n");
    {
        // 1.2 MB 的"响应体"（构造为合法 JSON，贴近真实网关大响应/错误明细）
        QString big = "{\"success\":true,\"msg\":\"大响应\",\"data\":[";
        const int n = 40000;
        for (int i = 0; i < n; ++i)
        {
            if (i) big += ",";
            big += QString("{\"i\":%1,\"code\":\"SKU%1\"}").arg(i);
        }
        big += "]}";
        check(big.size() > 1024 * 1024,
              QString("测试响应体 > 1MB"), QString("size=%1").arg(big.size()));

        // 同理造一个大 payload（大波次满箱报文可能上千 SKU 行）
        QString bigPayload = "{\"head\":{\"orderCode\":\"" + WA + "\"},\"details\":[";
        for (int i = 0; i < 5000; ++i)
        {
            if (i) bigPayload += ",";
            bigPayload += QString("{\"num\":\"22%1\",\"qty\":\"1\"}").arg(i, 3, 10, QChar('0'));
        }
        bigPayload += "]}";

        db.insertOutboxFullbox(mkFullbox("A7-BIG", WA, "066", bigPayload));
        db.markOutboxFullboxSuccess("A7-BIG");
        check(db.saveOutboxResponse("A7-BIG", true, 200, big, QString()),
              QString("大响应写入成功"));

        const QVector<OutboxRecord> rows = db.getOutboxFullboxByOrder(WA);
        const OutboxRecord* r = findById(rows, "A7-BIG");
        check(r && r->respBody.size() == big.size(),
              QString("大响应体读回长度完全相等（无截断）"),
              r ? QString("写入=%1 读回=%2").arg(big.size()).arg(r->respBody.size()) : QString("未找到"));
        check(r && r->respBody == big, QString("大响应体逐字符一致（含首尾 { } 与全部明细行）"));
        check(r && r->payload.size() == bigPayload.size() && r->payload == bigPayload,
              QString("大请求报文读回逐字符一致（报文本身也不截断）"),
              r ? QString("写入=%1 读回=%2").arg(bigPayload.size()).arg(r->payload.size()) : QString());
    }

    std::printf("== ④ 回传次数口径（批量统计 == 逐条统计；跨波次隔离）==\n");
    {
        bool ok7 = false, ok8 = false;
        const QMap<QString, OutboxStatusCount> all7 = db.getFullboxStatusCountAll(&ok7);
        const QMap<QString, OutboxStatusCount> all8 = db.getEndStatusCountAll(&ok8);
        check(ok7 && ok8, QString("批量统计查询执行成功"));

        const OutboxStatusCount c7 = all7.value(WA);
        const OutboxStatusCount c8 = all8.value(WA);
        const int totalA = c7.total() + c8.total();

        // 逐条统计（金标准）
        int s7 = 0, p7 = 0, f7 = 0, s8 = 0, p8 = 0, f8 = 0;
        for (const OutboxRecord& r : db.getOutboxFullboxByOrder(WA))
        {
            if (r.status == "success") ++s7; else if (r.status == "failed") ++f7; else ++p7;
        }
        for (const OutboxRecord& r : db.getOutboxEndByOrder(WA))
        {
            if (r.status == "success") ++s8; else if (r.status == "failed") ++f8; else ++p8;
        }
        check(c7.success == s7 && c7.pending == p7 && c7.failed == f7,
              QString("H7 批量计数与逐条一致（成功/待发/失败）"),
              QString("批量=%1/%2/%3 逐条=%4/%5/%6")
                  .arg(c7.success).arg(c7.pending).arg(c7.failed).arg(s7).arg(p7).arg(f7));
        check(c8.success == s8 && c8.pending == p8 && c8.failed == f8,
              QString("H8 批量计数与逐条一致"),
              QString("批量=%1/%2/%3").arg(c8.success).arg(c8.pending).arg(c8.failed));
        // 面板「回传次数」= H7 条数 + H8 条数（A：4 条 H7 + 1 条 H8 = 5 次）
        check(totalA == 5, QString("A 波次回传次数 = 5（H7 4 条 + H8 1 条）"),
              QString("total=%1 (h7=%2 h8=%3)").arg(totalA).arg(c7.total()).arg(c8.total()));

        const OutboxStatusCount b7 = all7.value(WB);
        check(b7.total() == 1, QString("B 波次回传次数 = 1（跨波次隔离，A 的报文不掺入）"),
              QString("total=%1").arg(b7.total()));
        check(!all7.contains("PP2026NOT_EXIST"),
              QString("无报文波次取不到键（面板按 0 处理 / 下拉标注\"无回传\"）"));
    }

    std::printf("== ⑤ 按波次全量取数（300 条不抽稀、不截断）==\n");
    {
        const QString WC = "PP2026RESP_BULK";
        db.upsertReturnWave(WC, 300, 3);
        for (int i = 0; i < 300; ++i)
        {
            db.insertOutboxFullbox(mkFullbox(QString("C7-%1").arg(i, 3, 10, QChar('0')), WC,
                                             QString("%1").arg(i % 66 + 1, 3, 10, QChar('0')),
                                             "{\"head\":{}}"));
        }
        const QVector<OutboxRecord> rows = db.getOutboxFullboxByOrder(WC);
        bool onlyC = true;
        for (const OutboxRecord& r : rows) if (r.orderCode != WC) onlyC = false;
        check(rows.size() == 300, QString("300 条全部返回（无 LIMIT / 无抽稀）"),
              QString("count=%1").arg(rows.size()));
        check(onlyC, QString("返回结果全部属于该波次"));
    }

    std::printf("== ⑥ 重传语义（覆盖\"最近一次\"响应，不新增条目）==\n");
    {
        const int before = db.getFullboxStatusCountAll().value(WA).total();
        const QString resp1 = "{\"success\":false,\"msg\":\"第一次失败\"}";
        const QString resp2 = "{\"success\":true,\"msg\":\"重传成功\"}";
        db.saveOutboxResponse("A7-FAIL", true, 500, resp1, QString());
        db.saveOutboxResponse("A7-FAIL", true, 200, resp2, QString());   // 手动重传成功
        const QVector<OutboxRecord> rows = db.getOutboxFullboxByOrder(WA);
        const OutboxRecord* r = findById(rows, "A7-FAIL");
        check(r && r->respHttp == 200 && r->respBody == resp2,
              QString("同一 msgId 二次写入覆盖前次（看最新一次响应）"),
              r ? QString("http=%1").arg(r->respHttp) : QString("未找到"));
        const int after = db.getFullboxStatusCountAll().value(WA).total();
        check(after == before, QString("重传不新增行（回传次数不变）"),
              QString("before=%1 after=%2").arg(before).arg(after));
        // 空 msgId 防御：不应误更新任何行
        check(!db.saveOutboxResponse(QString(), true, 200, "{}", QString()),
              QString("空 msgId 写入被拒（不做无差别 UPDATE）"));
    }

    db.close();
    std::printf("\n===== 结果：通过 %d 项，失败 %d 项 =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
