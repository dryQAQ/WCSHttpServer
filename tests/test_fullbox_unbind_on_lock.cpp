// ============================================================================
// test_fullbox_unbind_on_lock.cpp — ★ 2026-09-22 现场需求（可执行自测，非产品代码）
//
// 现场问题（第2条）：格口没有绑定容器（箱子已被取走/格口已打开），衣服照样落进去。
// 根因：PLC 锁格（=满箱）链路只发 H7 + 禁用格口，**完全不清容器号**；容器号只在 H7
//   回传"成功"时才被清掉 ⇒ H7 失败/被拒/被跳过时，内存与 DB 都残留一个物理上已不在场的
//   旧箱号 ⇒ 门禁"已解锁且已绑定容器"被骗过，件继续被导向没有箱子的格口（且落格写库串箱）。
//
// 现场确认的口径（本次实现）：
//   R1 锁格（=满箱）/完结补发时**立即保存箱号快照并清掉活跃绑定**，不等 H7 回传结果；
//   R2 H7 报文与箱号**即使失败也保留**：失败自动重试，重试耗尽落 failed 并等人工重传；
//   R3 完结回传（H8）**一定是最后一条报文**：点「结束任务」= 所有有数据的格口统一补发一遍 H7
//      → 固定延迟（endReportDelayMs，默认 1000ms）→ 不论 H7 是否成功都发 H8（结果与 H8 完全分开）。
//
// ★ 2026-09-25 现场口径更正：**解锁不发送满箱回传（H7）——只有锁格才发送**。
//   原实现：解锁时若该格仍有未上传记录（锁格时波次非执行态被跳过），按当前绑定补发一次 H7。
//   现场要求取消 ⇒ 解锁只刷新状态 + 留痕提示；遗留记录仍留给锁格 / 一键满箱 / 手动满箱 /
//   结束任务统一补发（flushUnreportedFullboxes）上传，见本文件 ⑮ 契约断言。
//
// ★ 2026-09-26 现场口径（B 方案）：**「H6 到场」≠「马上能收件」** —— PLC 仍处满箱锁格
//   （S7 锁格位=1，换箱动作未结束）时，H6 不立即解除该格禁用，恢复收件推迟到解锁边沿；
//   解锁到达且该格已绑定容器时才恢复（未换箱的格口保持"已解锁·待重绑"红）。
//   见本文件 ⑯ 契约断言（现场实例：H6 08:26:55.023 到、锁格位 08:26:56.497 才复位）。
//
// 本测试锁定两件事（可用 /c 编译、可单跑）：
//   ① 数据层契约（真实 SortingDatabase）：
//      · archiveGridBinds = active 绑定消失 + 历史行保留（unbind_time 留痕，箱号可追溯）；
//      · H7 报文与箱号在"pending → failed"全过程中**都在库里**（payload/boxcode 逐字不变），
//        即"失败也要保留好以便后续回传重试/人工重传"；
//      · pending 语义：next_retry 到点才可被 getPendingOutboxFullbox 取出；failed 不再进 pending。
//   ② 源码契约（防回归，读产品源码断言）：
//      · 锁格链路调用 detachContainerOnFullbox(..., "锁格满箱(H7待回传)")；
//      · 完结补发链路调用 detachContainerOnFullbox(..., "完结补发(H7待回传)")；
//      · lookupGridBoxCode 使用离场快照（boxSnapshotOf）；H6 绑定时清快照；
//      · 5 处"清空绑定"都同时清快照（m_boxSnapshot.clear()）；
//      · pollOutboxFullbox 不再把非当前波次的 pending 标 cancelled；
//      · sendEndToWms 只在 emitEndReportNow() 里被调用（H8 唯一出口 ⇒ H8 必然最后）；
//      · 锁格是**唯一**的自动 H7 触发点（sendFullbox(grid) 全源码仅 1 处）；
//      · 解锁链路不调用 sendFullbox（解锁不发满箱回传，只留痕提示）；
//      · H8 成功后：仅当该波次 H7 全成功才停 H7 重试轮询；
//      · 三个配置键在 define.h / ConfigManager.cpp 读写齐备，MainWindow 接线提示人工决策。
//
// 构建：见 tests\run_tests.bat 第 [13/13] 步
// ============================================================================

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QTextStream>
#include <QSqlDatabase>
#include <QSqlQuery>
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

// ── 源码读取（用于源码契约断言）──
static QString readSource(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

// 取函数体文本：从签名起，到下一个"顶层函数结束"（行首 '}'）为止（粗略但足够做契约断言）
static QString functionBody(const QString& src, const QString& signature, int maxLines = 400)
{
    const int at = src.indexOf(signature);
    if (at < 0) return QString();
    const QString tail = src.mid(at);
    const QStringList lines = tail.split('\n');
    QString out;
    for (int i = 0; i < lines.size() && i < maxLines; ++i)
    {
        out += lines[i] + "\n";
        if (i > 0 && lines[i].startsWith('}')) break;
    }
    return out;
}

static OutboxRecord mkFullbox(const QString& msgId, const QString& orderCode,
                              const QString& grid, const QString& box, const QString& payload)
{
    OutboxRecord m;
    m.msgId     = msgId;
    m.orderCode = orderCode;
    m.boxcode   = box;
    m.grid      = grid;
    m.payload   = payload;
    m.status    = "pending";
    m.retryCount = 0;
    const QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    m.nextRetry = now;
    m.createdAt = now;
    return m;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString order = "PP2026LOCKTEST";
    const QString dbPath = QDir::currentPath() + "/test_fullbox_unbind_on_lock.db";
    QFile::remove(dbPath);

    SortingDatabase& db = SortingDatabase::instance();
    if (!db.open(dbPath))
    {
        std::printf("[FAIL] SortingDatabase open failed: %s\n", dbPath.toUtf8().constData());
        return 1;
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ① 数据层：锁格即清活跃绑定，历史行保留（箱号可追溯）==\n");
    // ════════════════════════════════════════════════════════════════════
    // ★ 2026-09-22 性能（现场反馈"点开按钮界面明显卡顿"）：确认两个慢查询所需的索引已自动建立
    //   （波次列表 getAllWaves 对 exception_record 每波次全表扫描；H8 按波次取数）——旧库打开即补建。
    {
        QSqlDatabase probe = QSqlDatabase::addDatabase("QSQLITE", "probe_idx_check");
        probe.setDatabaseName(dbPath);
        if (probe.open())
        {
            QSqlQuery q(probe);
            QStringList idx;
            if (q.exec("PRAGMA index_list(exception_record)"))
                while (q.next()) idx << q.value(1).toString();
            check(idx.contains("idx_exc_order"),
                  "索引 idx_exc_order ON exception_record(order_code) 已自动建立（波次列表提速）",
                  idx.join(","));
            QStringList idx2;
            if (q.exec("PRAGMA index_list(outbox_end)"))
                while (q.next()) idx2 << q.value(1).toString();
            check(idx2.contains("idx_outbox_end_order"),
                  "索引 idx_outbox_end_order ON outbox_end(order_code) 已自动建立（H8 按波次取数提速）",
                  idx2.join(","));
            probe.close();
        }
        QSqlDatabase::removeDatabase("probe_idx_check");
    }
    check(db.bindGridBox("036", "H-T0461", order), "H6 绑定写入 grid=036 box=H-T0461");
    {
        GridBoxBindRecord a = db.getActiveBind("036");
        check(a.boxcode == "H-T0461", "绑定后 getActiveBind(036) = H-T0461",
              a.boxcode);
    }
    check(db.archiveGridBinds("036"), "archiveGridBinds(036) 执行成功（= 锁格即解绑动作）");
    {
        GridBoxBindRecord a = db.getActiveBind("036");
        check(a.boxcode.isEmpty(), "解绑后该格口再无 active 绑定（门禁视为未绑定 ⇒ 件改投异常口）",
              a.boxcode);
    }
    {
        const QVector<GridBoxBindRecord> hist = db.getLastBindsByOrder(order);
        bool found = false, hasUnbind = false;
        for (const GridBoxBindRecord& r : hist)
        {
            if (r.gridNum.compare("036", Qt::CaseInsensitive) == 0 || r.gridNum.toInt() == 36)
            {
                found = true;
                if (!r.unbindTime.isEmpty()) hasUnbind = true;
            }
        }
        check(found && hasUnbind,
              "历史绑定行保留且带 unbind_time 留痕（箱号可追溯，行不删除）",
              QString("found=%1 unbind=%2").arg(found).arg(hasUnbind));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("\n== ② 数据层：H7 报文与箱号在 pending→failed 全过程中保留（可重试/可人工重传）==\n");
    // ════════════════════════════════════════════════════════════════════
    const QString payload = QString::fromUtf8("{\"head\":{\"detailList\":[{\"num\":\"22036\","
                                              "\"qty\":\"75\",\"sku\":\"116107036601804\","
                                              "\"targetLocation\":\"H-T0461\"}]}}");
    check(db.insertOutboxFullbox(mkFullbox("MSG036", order, "036", "H-T0461", payload)),
          "H7 报文入 Outbox（pending）");
    {
        const QVector<OutboxRecord> rows = db.getOutboxFullboxByOrder(order);
        check(rows.size() == 1 && rows[0].boxcode == "H-T0461" && rows[0].payload == payload,
              "报文与箱号落库后逐字可读（payload/boxcode 不变）",
              QString("rows=%1").arg(rows.size()));
    }
    {
        const QVector<OutboxRecord> pend = db.getPendingOutboxFullbox(10);
        bool found = false;
        for (const OutboxRecord& r : pend) if (r.msgId == "MSG036") found = true;
        check(found, "next_retry 到点的 pending 报文可被重试调度取出");
    }
    check(db.updateOutboxFullboxStatus("MSG036", "failed",
              QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")),
          "重试耗尽 → 置 failed（记录留痕，等人工回传）");
    {
        const QVector<OutboxRecord> rows = db.getOutboxFullboxByOrder(order);
        bool kept = false;
        for (const OutboxRecord& r : rows)
            if (r.msgId == "MSG036" && r.status == "failed" && r.payload == payload
                && r.boxcode == "H-T0461")
                kept = true;
        check(kept, "failed 报文仍在库中且 payload/boxcode 完整（人工重传依据）",
              QString("rows=%1").arg(rows.size()));
    }
    {
        const QVector<OutboxRecord> pend = db.getPendingOutboxFullbox(10);
        bool found = false;
        for (const OutboxRecord& r : pend) if (r.msgId == "MSG036") found = true;
        check(!found, "failed 不再进入 pending（不会无限重发）");
    }
    check(db.markOutboxFullboxSuccess("MSG036"), "人工重传成功后置 success（条目保留，供审计）");
    {
        const QVector<OutboxRecord> rows = db.getOutboxFullboxByOrder(order);
        bool ok = false;
        for (const OutboxRecord& r : rows)
            if (r.msgId == "MSG036" && r.status == "success") ok = true;
        check(ok, "success 状态落库（重传成功后状态可追溯）");
    }

    db.close();

    // ════════════════════════════════════════════════════════════════════
    std::printf("\n== ②B 数据层：每次失败回传独立消耗一次重试次数（按 msgId 独立、终态不重复计）==\n");
    // ════════════════════════════════════════════════════════════════════
    {
        SortingDatabase& d2 = SortingDatabase::instance();
        const QString dbPath2 = QDir::currentPath() + "/test_fullbox_retry_count.db";
        QFile::remove(dbPath2);
        if (!d2.open(dbPath2))
        {
            std::printf("[FAIL] retry-count DB open failed\n");
            return 1;
        }

        const QString o = "PP2026RETRYTEST";
        const QString p = "{\"head\":{\"detailList\":[]}}";
        d2.insertOutboxFullbox(mkFullbox("RTA", o, "036", "H-TA", p));
        d2.insertOutboxFullbox(mkFullbox("RTB", o, "052", "H-TB", p));

        // ① 初始 = 0（首发不计入重试次数）
        check(d2.getOutboxFullboxByMsgId("RTA").retryCount == 0
                  && d2.getOutboxFullboxByMsgId("RTB").retryCount == 0,
              "首发不计次：两条报文初始 retry_count 均为 0");

        // ② 每次"重发调度"独立 +1（且只影响自己那条）
        const QString nxt = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        d2.updateOutboxFullboxStatus("RTA", "pending", nxt);
        d2.updateOutboxFullboxStatus("RTA", "pending", nxt);
        d2.updateOutboxFullboxStatus("RTB", "pending", nxt);
        check(d2.getOutboxFullboxByMsgId("RTA").retryCount == 2,
              "RTA 重发 2 次 → retry_count=2（每次失败重发各消耗一次）",
              QString::number(d2.getOutboxFullboxByMsgId("RTA").retryCount));
        check(d2.getOutboxFullboxByMsgId("RTB").retryCount == 1,
              "RTB 重发 1 次 → retry_count=1（按 msgId 独立，不被 RTA 影响）",
              QString::number(d2.getOutboxFullboxByMsgId("RTB").retryCount));

        // ③ 终态标记（重试耗尽/取消/payload 失败）**不再 +1**
        d2.setOutboxFullboxStatus("RTA", "failed", nxt);
        {
            OutboxRecord a = d2.getOutboxFullboxByMsgId("RTA");
            check(a.status == "failed" && a.retryCount == 2,
                  "终态标记 failed 只改状态：retry_count 仍为 2（不出现 3/2 的口径漂移）",
                  QString("status=%1 retry=%2").arg(a.status).arg(a.retryCount));
        }

        // ④ 成功标记不计数
        d2.markOutboxFullboxSuccess("RTB");
        {
            OutboxRecord b = d2.getOutboxFullboxByMsgId("RTB");
            check(b.status == "success" && b.retryCount == 1,
                  "成功标记只改状态：retry_count 仍为 1（成功的那次重发已计过）",
                  QString("status=%1 retry=%2").arg(b.status).arg(b.retryCount));
        }

        // ⑤ H8 同口径
        OutboxRecord e;
        e.msgId = "RTE"; e.orderCode = o; e.payload = p;
        e.nextRetry = nxt; e.createdAt = nxt;
        d2.insertOutboxEnd(e);
        d2.updateOutboxEndStatus("RTE", "pending", nxt);      // 一次重发
        d2.setOutboxEndStatus("RTE", "failed", nxt);          // 终态
        {
            OutboxRecord r = d2.getOutboxEndByMsgId("RTE");
            check(r.status == "failed" && r.retryCount == 1,
                  "H8 同口径：重发 1 次 + 终态 failed → retry_count=1",
                  QString("status=%1 retry=%2").arg(r.status).arg(r.retryCount));
        }

        // ⑥ 上限兜底可用：达上限的报文可被置 failed（模拟 poll 的上限分支）
        d2.updateOutboxFullboxStatus("RTB", "pending", nxt);
        d2.setOutboxFullboxStatus("RTB", "failed", nxt);
        {
            OutboxRecord b = d2.getOutboxFullboxByMsgId("RTB");
            check(b.status == "failed" && b.retryCount == 2,
                  "达上限后置 failed 且计数保持（回执丢失也能收口，不再无限重发）",
                  QString("status=%1 retry=%2").arg(b.status).arg(b.retryCount));
        }

        d2.close();
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("\n== ③ 源码契约：锁格即保存+清理 / 快照兜底 / H8 唯一出口 / 重试保留 ==\n");
    // ════════════════════════════════════════════════════════════════════
    const QString root = QDir::currentPath() + "/..";
    const QString cppPath = root + "/WCS_httpServer/HttpServer.cpp";
    const QString hPath   = root + "/WCS_httpServer/HttpServer.h";
    const QString mwPath  = root + "/WCS_httpServer/MainWindow.cpp";
    const QString cfgPath = root + "/WCS_httpServer/ConfigManager.cpp";
    const QString defPath = root + "/WCS_httpServer/define.h";

    const QString cpp = readSource(cppPath);
    const QString hdr = readSource(hPath);
    const QString mw  = readSource(mwPath);
    const QString cfg = readSource(cfgPath);
    const QString def = readSource(defPath);

    check(!cpp.isEmpty() && !hdr.isEmpty(), "产品源码可读（契约断言前置）",
          cppPath);
    if (cpp.isEmpty() || hdr.isEmpty()) { std::printf("[FAIL] source not readable\n"); return 1; }

    // ① 锁格链路：快照 + 立即清理
    {
        const QString lockBody = functionBody(cpp, "&PlcManager::gridLocked", 90);
        check(lockBody.contains("markBoxSnapshot"), "锁格链路写入离场箱号快照（markBoxSnapshot）");
        check(lockBody.contains("锁格满箱(H7待回传)"), "锁格链路调用 detachContainerOnFullbox(原因=锁格满箱(H7待回传))");
        check(lockBody.contains("sortingClearBoxOnFullbox"), "锁格链路受配置开关 sortingClearBoxOnFullbox 控制（可一键回退）");
        check(lockBody.indexOf("sendFullbox(grid)") < lockBody.indexOf("detachContainerOnFullbox"),
              "顺序正确：先发 H7（带快照箱号）→ 再清绑定（不等回执）");
    }
    // ② 完结补发链路：同口径（★ 2026-09-26 起 = 「一键满箱回传」同一实现 reportFullboxAllBoundGrids）
    {
        const QString flushBody = functionBody(cpp, "int HttpServer::flushUnreportedFullboxes", 90);
        const QString batchBody = functionBody(cpp, "QVector<FullboxBatchEntry> HttpServer::reportFullboxAllBoundGrids", 70);
        check(flushBody.contains("reportFullboxAllBoundGrids") && flushBody.contains("detachOnSuccess"),
              "结束任务补发 = 已绑定容器逐个 H7（与「一键满箱回传」共用 reportFullboxAllBoundGrids）");
        check(batchBody.contains("完结补发(H7待回传)") && batchBody.contains("detachOnSuccess"),
              "完备结口径清理（原因=完结补发(H7待回传)）：仅结束任务传 detachOnSuccess=true");
        check(batchBody.contains("getContainerBindings()"),
              "补发集合 = 当前已绑定容器（箱号取当前绑定；离场旧箱走\u201c跳过+留痕\u201d）");
        check(flushBody.contains("leftoverDesc") && flushBody.contains("满箱回传未完成"),
              "未绑定容器但有未上传记录的格口：跳过 + 异常表留痕（格口号/条数/手输格口号补传指引）");
    }
    // ③ 快照兜底 + H6 清除
    {
        const QString lookupBody = functionBody(cpp, "QString HttpServer::lookupGridBoxCode", 45);
        check(lookupBody.contains("boxSnapshotOf(grid)"),
              "箱号查找链加入离场快照兜底（内存绑定 → 快照 → DB）");
        const QString bindBody = functionBody(cpp, "QJsonObject HttpServer::handleBindingLatticePort", 200);
        check(bindBody.contains("m_boxSnapshot.remove("),
              "H6 绑定成功清除离场快照（新容器到场 ⇒ 恢复可下发）");
    }
    // ④ 五处清空绑定都清快照
    {
        int n = cpp.count("m_boxSnapshot.clear()");
        check(n >= 5, "5 处批量清空绑定同时清空快照（启动/清空/切出/新任务/取消）",
              QString("count=%1").arg(n));
    }
    // ⑤ H7 重试保留：不再 cancelled
    {
        const QString pollBody = functionBody(cpp, "void HttpServer::pollOutboxFullbox", 80);
        check(!pollBody.contains("\"cancelled\""),
              "pollOutboxFullbox 不再把非当前波次 pending 标 cancelled（报文保留可重试）");
        check(pollBody.contains("保留可重试"), "重试调度保留历史波次报文的语义注释在场");
    }
    // ⑥ H8 唯一构造点 + 唯一发送出口 ⇒ H8 必然最后
    {
        const QString emitBody    = functionBody(cpp, "bool HttpServer::emitEndReportNow", 140);
        const QString sendEndBody = functionBody(cpp, "bool HttpServer::sendEnd()", 170);
        const QString pollEndBody = functionBody(cpp, "void HttpServer::pollOutboxEnd", 120);
        check(cpp.count("insertOutboxEnd(") == 1 && emitBody.contains("insertOutboxEnd("),
              "H8 报文只在 emitEndReportNow() 中构造（sendEnd 不再各自造报文）",
              QString("insertOutboxEnd=%1").arg(cpp.count("insertOutboxEnd(")));
        check(!sendEndBody.contains("insertOutboxEnd(")
                  && sendEndBody.contains("flushUnreportedFullboxes")
                  && sendEndBody.contains("m_endDelayTimer->start(delayMs)"),
              "sendEnd()：先统一补发 H7 → 启动固定延迟定时器（不再直接发 H8）");
        check(cpp.contains("void HttpServer::onEndDelayTimeout()")
                  && functionBody(cpp, "void HttpServer::onEndDelayTimeout()", 120).contains("emitEndReportNow()"),
              "固定延迟到点（onEndDelayTimeout）强制发送 H8（不论 H7 是否成功）");
        check(pollEndBody.contains("msg.payload") && pollEndBody.contains("sendEndToWms("),
              "pollOutboxEnd 仅重发**已入库**的 H8 报文（重试不改顺序：H8 早已在 H7 之后）");
        check(cpp.count("sendEndToWms(") == 3,
              "sendEndToWms 出现 3 处 = 定义 1 + 首次发送(emitEndReportNow) 1 + 存量重试(pollOutboxEnd) 1",
              QString("occurrences=%1").arg(cpp.count("sendEndToWms(")));
    }
    // ⑦ 结果与完结回传**完全分开**：屏障已取消（H7 回执/人工重传都不再触发或影响 H8）
    {
        const QString fbBody = functionBody(cpp, "void HttpServer::onFullboxReplyFinished", 400);
        check(!fbBody.contains("emitEndReportNow()"),
              "H7 回执不再触发完结回传（H7 结果与 H8 分开）");
        const QString rsBody = functionBody(cpp, "void HttpServer::onOutboxResendReply", 140);
        check(!rsBody.contains("emitEndReportNow()"),
              "人工重传 H7 的结果同样不触发/不影响完结回传");
        check(!cpp.contains("EndBarrier") && !hdr.contains("EndBarrier"),
              "完结屏障代码已整体移除（无 classifyEndBarrier/checkEndBarrier/holdEndReport 等残渣）");
        check(cpp.contains("满箱回传未完成"),
              "未成功清单仍写异常表留痕（type=满箱回传未完成，纯记录不改发送）");
        check(!mw.contains("endBarrierNeedDecision") && !mw.contains("先去处理"),
              "UI 不再有「先去处理/确认直接完结」完结弹窗（弹窗随屏障一并移除）");
    }
    // ⑧ H8 成功后仍保留 H7 重试（有未成功报文时）
    {
        const QString endBody = functionBody(cpp, "void HttpServer::onEndReplyFinished", 500);
        check(endBody.contains("bHasUnfinishedH7"),
              "H8 成功后按『是否仍有未成功 H7』决定是否停重试轮询（保留自动重试）");
    }
    // ⑨ 配置键齐备
    {
        check(def.contains("SORTING_CLEAR_BOX_ON_FULLBOX") && def.contains("END_REPORT_DELAY_MS"),
              "define.h 提供锁格清容器开关 + 完结回传固定延迟默认值");
        check(cfg.count("sortingClearBoxOnFullbox") >= 2 && cfg.count("endReportDelayMs") >= 2,
              "ConfigManager 读写齐备（load + save 各一处以上）");
        check(cfg.contains("已废弃") && cfg.contains("endFullboxBarrier"),
              "旧键 endFullboxBarrier/Policy 保留读取分支但只提示「已废弃」（不影响行为）");
    }
    // ⑩ 重试计数口径（每次失败回传独立消耗一次）
    {
        const QString pollBody   = functionBody(cpp, "void HttpServer::pollOutboxFullbox", 120);
        const QString pollEndBody= functionBody(cpp, "void HttpServer::pollOutboxEnd", 120);
        const QString resendBody = functionBody(cpp, "void HttpServer::onOutboxResendReply", 140);
        const QString fullboxBody= functionBody(cpp, "void HttpServer::onFullboxReplyFinished", 400);
        const QString endBody    = functionBody(cpp, "void HttpServer::onEndReplyFinished", 500);
        check(pollBody.contains("msg.retryCount >= OUTBOX_RETRY_MAX_H7")
                  && pollBody.contains("setOutboxFullboxStatus(msg.msgId, \"failed\""),
              "H7 重试调度含上限兜底：达上限直接置 failed（回执丢失也不再无限重发）");
        check(pollEndBody.contains("msg.retryCount >= OUTBOX_RETRY_MAX_DEFAULT")
                  && pollEndBody.contains("setOutboxEndStatus(msg.msgId, \"failed\""),
              "H8 重试调度含上限兜底：达上限直接置 failed");
        check(pollBody.contains("updateOutboxFullboxStatus(msg.msgId, \"pending\"")
                  && pollEndBody.contains("updateOutboxEndStatus(msg.msgId, \"pending\""),
              "重发调度用 +1 版本（每次实际重发 = 消耗一次重试次数）");
        check(resendBody.contains("updateOutboxFullboxStatus(msgId, keepStatus")
                  && resendBody.contains("updateOutboxEndStatus(msgId, keepStatus"),
              "人工重传失败也消耗一次重试次数（保持原状态不变，只 +1）");
        check(fullboxBody.contains("setOutboxFullboxStatus(msgId, \"failed\"")
                  && !fullboxBody.contains("updateOutboxFullboxStatus(msgId, \"failed\""),
              "H7 终态标记（重试耗尽）不再 +1（用 set 版本，计数口径不漂移）");
        check(endBody.contains("setOutboxEndStatus(msgId, \"failed\"")
                  && !endBody.contains("updateOutboxEndStatus(msgId, \"failed\""),
              "H8 终态标记（重试耗尽）不再 +1（用 set 版本）");
    }
    // ⑪ 回传结果与容器绑定**彻底解耦**（现场口径：只在锁格/补发时清容器号，回传结果独立）
    {
        const QString fbBody  = functionBody(cpp, "void HttpServer::onFullboxReplyFinished", 400);
        const QString rsBody  = functionBody(cpp, "void HttpServer::onOutboxResendReply", 160);
        const QString pollBody= functionBody(cpp, "void HttpServer::pollOutboxFullbox", 130);
        check(!fbBody.contains("detachContainer") && !fbBody.contains("archiveGridBinds")
                  && !fbBody.contains("m_containerBindings.erase"),
              "H7 回传成功：不碰容器绑定（不解绑、不归档、不改内存绑定）——结果独立");
        check(!rsBody.contains("detachContainer") && !rsBody.contains("archiveGridBinds")
                  && !rsBody.contains("m_containerBindings.erase"),
              "人工重传成功：同样不碰容器绑定");
        check(!pollBody.contains("detachContainer") && !pollBody.contains("archiveGridBinds"),
              "H7 重试调度：同样不碰容器绑定");
        check(fbBody.contains("结果仅更新报文状态") || fbBody.contains("解耦"),
              QString::fromUtf8("H7 成功分支留有「结果不影响容器绑定」的语义说明"));
        check(!cpp.contains("detachContainerIfStillBound"),
              QString::fromUtf8("已删除「回传成功后兜底解绑」链路（不再存在能因回执清容器的代码路径）"));
        check(functionBody(cpp, "&PlcManager::gridLocked", 90).contains("detachContainerOnFullbox")
                  && functionBody(cpp, "QVector<FullboxBatchEntry> HttpServer::reportFullboxAllBoundGrids", 70)
                         .contains("detachContainerOnFullbox"),
              "容器号只在锁格（=满箱）与结束任务补发这一刻清理（发送时刻，不等回执；按钮版不清）");
    }
    // ⑫ 异常类型中文表述 + 「信息不全」说明缺什么（现场要求）
    {
        const QString mw = readSource(QDir::currentPath() + "/../WCS_httpServer/MainWindow.cpp");
        check(mw.contains("static QString exceptionTypeLabel(const QString& type)"),
              "存在统一的「异常类型中文」翻译函数（多处弹窗共用一份对照表）");
        const QStringList codes = {"plc_no_grid", "plc_info_incomplete", "no_bind", "no_match", "wrong_grid"};
        bool allMapped = true;
        for (const QString& c : codes)
            if (!mw.contains(QString("\"%1\")").arg(c))) allMapped = false;
        check(allMapped, "内部代号全部有中文对照（无格口/信息不全/未绑定容器/无匹配计划格口/落错格口）");
        check(!mw.contains("<< QString::number(i + 1) << e.time << e.type"),
              "「查看处理」异常类型列不再直接显示内部代号（改用中文标签）");
        check(!mw.contains(".arg(e.time, e.type)"),
              "「导出到运行日志」同样输出中文类型");
        check(mw.count("exceptionTypeLabel(") >= 4,
              "所有展示点（查看处理/EPC全信息/实时面板/导出）统一走中文标签",
              QString("count=%1").arg(mw.count("exceptionTypeLabel(")));
        check(!mw.contains("plc_no_grid / plc_info_incomplete"),
              "界面帮助文案里不再出现英文代号（改为 PLC反馈无格口 / PLC反馈信息不全）");
        // 「信息不全」必须说明缺什么（服务端按 5 字段格式逐项判断并写入原因）
        check(cpp.contains("PLC 反馈 status=3（信息不全）：缺少 %1")
                  && cpp.contains("\"EPC/条码\"") && cpp.contains("\"格口号\"")
                  && cpp.contains("\"小车号\"") && cpp.contains("\"尾车号\""),
              "「信息不全」写明缺少哪个字段（EPC/条码、格口号、小车号、尾车号）");
        check(cpp.contains("具体缺失项由 PLC 侧判定"),
              "字段看似齐全时不臆造原因，提示对照 PLC.log 的 raw 行核对");
        check(cpp.contains("exRec.type      = (e.status == 2) ? \"plc_no_grid\" : \"plc_info_incomplete\""),
              "库内 type 代号保持不变（处理数统计口径不受显示层影响）");
    }
    // ⑬ 现场口径（最终定稿）：点「结束任务」= 全部格口统一补发 H7 → **固定 1 秒后强制发 H8**，
    //    不论 H7 是否成功；H7 结果与完结回传完全分开（完结屏障/策略/弹窗已取消）
    {
        const QString sendEndBody = functionBody(cpp, "bool HttpServer::sendEnd()", 260);
        const QString delayBody   = functionBody(cpp, "void HttpServer::onEndDelayTimeout()", 120);
        const QString cancelBody  = functionBody(cpp, "void HttpServer::cancelPendingEndReport(", 80);

        check(def.contains("#define END_REPORT_DELAY_MS 2000"),
              "define.h：完结回传固定延迟默认 2000ms（END_REPORT_DELAY_MS；2026-09-26 由 1 秒改 2 秒）");
        // ① 先统一补发：flush 在启动延迟之前
        const int iFlush = sendEndBody.indexOf("flushUnreportedFullboxes");
        const int iDelay = sendEndBody.indexOf("m_endDelayTimer->start(delayMs)");
        check(iFlush >= 0 && iDelay > iFlush,
              "顺序：先把所有格口统一补发满箱回传（H7），再启动完结回传延迟（H8 必定最后）",
              QString("flush@%1 delay@%2").arg(iFlush).arg(iDelay));
        // ② 延迟来自配置，且与 H7 结果无关（不看 classify/policy）
        check(sendEndBody.contains("endReportDelayMs")
                  && !sendEndBody.contains("classifyEndBarrier")
                  && !sendEndBody.contains("endFullboxBarrier")
                  && !sendEndBody.contains("emit endBarrierNeedDecision"),
              "延迟取自配置 endReportDelayMs；sendEnd 不做任何 H7 结果判定（结果分开）");
        check(sendEndBody.contains("不论满箱回传是否成功"),
              "sendEnd 注释/提示明示：不论满箱回传是否成功都会发 H8");
        // ③ 到点后**无条件**发送：只有归属/状态防护，没有"等 H7 成功才发"的判据
        check(delayBody.contains("emitEndReportNow()")
                  && !delayBody.contains("return;   // 仍有未成功")
                  && delayBody.contains("collectUnfinishedFullbox"),
              "延迟到点：统计未成功 H7 仅供留痕，随后无条件 emitEndReportNow()");
        check(delayBody.contains("writeUnfinishedFullboxNotice"),
              "未成功的 H7 写异常留痕（纯记录，不改变是否发送）");
        check(delayBody.contains("m_pWaveMgr->orderCode() != order")
                  && delayBody.contains("WAVE_ENDING"),
              "归属防护：延迟期间已切波次/状态非完结中则不再发送（报文仍可面板补发）");
        // ④ 取消路径（切出/取消）与收敛
        check(cancelBody.contains("m_endDelayTimer->stop()")
                  && cpp.contains("cancelPendingEndReport(QString::fromUtf8(\"波次切出\"))")
                  && cpp.contains("cancelPendingEndReport(QString::fromUtf8(\"波次取消(H5)\"))"),
              "波次切出/取消(H5) 会取消尚未到点的延迟发送（不再有屏障收敛语义）");
        // 幂等：1 秒窗口内重复点「结束任务」不重复补发、不误报失败
        check(sendEndBody.contains("m_endDelayTimer->isActive()")
                  && sendEndBody.contains("本次重复点击已忽略")
                  && sendEndBody.contains("return true;"),
              "重复点击「结束任务」：延迟排队中直接返回 true（不重复补发 H7、不误报触发失败）");
        // ⑤ 屏障残渣必须为零（现场口径：不再有策略/弹窗/人工决策）
        check(!cpp.contains("classifyEndBarrier") && !hdr.contains("classifyEndBarrier")
                  && !cpp.contains("holdEndReport") && !cpp.contains("confirmEndReport"),
              "完结屏障相关函数已整体删除（无 classify/check/tick/hold/confirm 残留）");
        check(!hdr.contains("endBarrierNeedDecision") && !hdr.contains("endBarrierChanged")
                  && !mw.contains("endBarrierNeedDecision"),
              "完结屏障信号与 UI 弹窗已移除（信号、接线、文案一并清掉）");
        // ⑥ 配置：新键 + 旧键废弃提示
        const QString cfg2 = readSource(QDir::currentPath() + "/../WCS_httpServer/ConfigManager.cpp");
        check(cfg2.contains("endReportDelayMs") && cfg2.contains("qBound(0,"),
              "配置项 endReportDelayMs 读取（0..60000ms 钳制，0=立即发）");
        const QString xml = readSource(QDir::currentPath() + "/../release_WcsHttpServer/config/http_server.xml");
        check(xml.contains("<endReportDelayMs>2000</endReportDelayMs>"),
              "现场配置：endReportDelayMs=2000（满箱补发完成后固定 2 秒强制发送完结回传）",
              xml.isEmpty() ? "配置文件不可读" : QString());
        check(!xml.contains("<endFullboxBarrier"),
              "现场配置已移除旧的完结屏障/策略键（endFullboxBarrier / Policy）");
    }

    // ⑭ 单格口满箱回传"一直失败"也不影响完结回传（H8）—— 现场要求：一个格口一直回传失败，不能影响完结回传
    {
        // ── 数据层：用独立库复现"某格口 H7 重试耗尽 failed"，同时该波次 H8 照常入库/成功 ──
        SortingDatabase& d3 = SortingDatabase::instance();
        const QString dbPath3 = QDir::currentPath() + "/test_fullbox_noblock.db";
        QFile::remove(dbPath3);
        if (!d3.open(dbPath3))
        {
            std::printf("[FAIL] noblock DB open failed\n");
            return 1;
        }
        const QString o3 = "PP2026NOBLOCK";
        const QString p3 = QString::fromUtf8("{\"head\":{\"detailList\":[{\"num\":\"22036\","
                                             "\"qty\":\"75\",\"sku\":\"116107036601804\","
                                             "\"targetLocation\":\"H-T0461\"}]}}");
        const QString nxt = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

        // 格口036：报文入 Outbox → 两次重发（各消耗一次）→ 重试耗尽 failed（"一直回传失败"的终态）
        check(d3.insertOutboxFullbox(mkFullbox("NBF", o3, "036", "H-T0461", p3)),
              "一直失败的格口：H7 报文入 Outbox（pending）");
        d3.updateOutboxFullboxStatus("NBF", "pending", nxt);
        d3.updateOutboxFullboxStatus("NBF", "pending", nxt);
        d3.setOutboxFullboxStatus("NBF", "failed", nxt);
        {
            const OutboxRecord f = d3.getOutboxFullboxByMsgId("NBF");
            check(f.status == "failed" && f.retryCount == 2,
                  "该格口 H7 重试耗尽 → failed（retry_count=2，不再重发）",
                  QString("status=%1 retry=%2").arg(f.status).arg(f.retryCount));
        }
        // 对照组：另一个格口仍有待重发报文（证明"取待重试"查询本身有效，NBF 不出现是状态而非查询坏了）
        d3.insertOutboxFullbox(mkFullbox("NBOK", o3, "052", "H-T0453", p3));
        {
            const QVector<OutboxRecord> pend = d3.getPendingOutboxFullbox(50);
            bool hasOk = false, revived = false;
            for (const OutboxRecord& r : pend)
            {
                if (r.msgId == "NBOK") hasOk = true;
                if (r.msgId == "NBF")  revived = true;
            }
            check(hasOk && !revived,
                  "failed 的报文不会被 H7 调度重新取回（不会无限重发；同批 pending 的照常取到）");
        }

        // 该波次 H8：独立入库、独立取数 —— 完全不受"某格口一直失败"影响
        check(d3.insertOutboxEnd(mkFullbox("NBEND", o3, "", "",
                  QString::fromUtf8("{\"head\":{\"orderCode\":\"PP2026NOBLOCK\"}}"))),
              "该波次 H8 报文可独立入 Outbox（不受该格口 H7 failed 影响）");
        {
            const QVector<OutboxRecord> ends = d3.getOutboxEndByOrder(o3);
            bool hasEnd = false;
            for (const OutboxRecord& r : ends) if (r.msgId == "NBEND") hasEnd = true;
            check(hasEnd, "H8 报文按波次可读（完结回传照常发出/可查）",
                  QString("ends=%1").arg(ends.size()));
        }
        {
            const OutboxRecord h7 = d3.getOutboxFullboxByMsgId("NBF");
            check(h7.status == "failed" && h7.boxcode == "H-T0461" && h7.payload == p3,
                  "写入 H8 不改写该格口 H7：仍是 failed、箱号与报文逐字保留（可人工补传）",
                  h7.status);
        }
        // H8 成功（完结回传完成）也不得动该格口的失败报文：报文与箱号必须留着等补传
        check(d3.markOutboxEndSuccess("NBEND"), "H8 标记成功（完结回传完成）");
        {
            const OutboxRecord h7 = d3.getOutboxFullboxByMsgId("NBF");
            check(h7.status == "failed" && h7.boxcode == "H-T0461" && h7.payload == p3,
                  "H8 成功后该格口失败报文仍在库中且内容不变（等人工重传，不被清掉）",
                  h7.status);
            const OutboxRecord h8 = d3.getOutboxEndByMsgId("NBEND");
            check(h8.status == "success", "H8 自身状态正确置成功（两条链路互不干扰）", h8.status);
        }
        // H8 的待重试队列里不得混入 H7 报文（两个调度互不串台）
        {
            const QVector<OutboxRecord> pendEnd = d3.getPendingOutboxEnd(50);
            bool mixedH7 = false;
            for (const OutboxRecord& r : pendEnd)
                if (r.msgId == "NBF" || r.msgId == "NBOK") mixedH7 = true;
            check(!mixedH7, "H8 重试队列不混入 H7 报文（完结回传只按 outbox_end 调度）");
        }

        // ── 源码契约：H8 的生成/发送/重试全程不读 H7 状态；H8 成功不要求 H7 全成功 ──
        const QString emitBody = functionBody(cpp, "bool HttpServer::emitEndReportNow()", 220);
        check(!emitBody.isEmpty()
                  && !emitBody.contains("getOutboxFullboxByOrder")
                  && !emitBody.contains("getPendingOutboxFullbox")
                  && !emitBody.contains("outbox_fullbox")
                  && !emitBody.contains("classifyEndBarrier"),
              "emitEndReportNow() 完全不读满箱回传状态 ⇒ 某格口一直失败也挡不住 H8 的生成与发送");
        const QString pollEndBody = functionBody(cpp, "void HttpServer::pollOutboxEnd()", 160);
        check(!pollEndBody.isEmpty()
                  && !pollEndBody.contains("Fullbox") && !pollEndBody.contains("fullbox"),
              "H8 重试调度（pollOutboxEnd）不依赖满箱回传结果（独立重试、独立上限）");
        const QString pollH7Body = functionBody(cpp, "void HttpServer::pollOutboxFullbox()", 160);
        check(!pollH7Body.isEmpty()
                  && !pollH7Body.contains("OutboxEnd")
                  && !pollH7Body.contains("setOutboxFullboxStatus(msg.msgId, \"cancelled\"")
                  && pollH7Body.contains("OUTBOX_RETRY_MAX_H7"),
              "H7 重试调度：不动 H8、不把保留报文标 cancelled、到上限即收口（不会无限刷）");
        // H8 成功分支：波次照样 FINISHED；未成功的 H7 只保留重试轮询，不做"必须全成功才完结"的判据
        const QString endReplyBody = functionBody(cpp, "void HttpServer::onEndReplyFinished(", 200);
        check(endReplyBody.contains("setState(WAVE_FINISHED)")
                  && endReplyBody.contains("保留重试轮询继续自动重试")
                  && !endReplyBody.contains("return;   // 仍有未成功的 H7"),
              "H8 成功 ⇒ 波次照常 FINISHED（个别格口 H7 未成功只保留后台重试，不阻挡完结）");
        // 完结补发循环：某个格口发失败不能中断整轮补发（否则后面的格口不会补发、连带影响 H8 前的一致性）
        const QString flushBody = functionBody(cpp, "int HttpServer::flushUnreportedFullboxes(", 80);
        check(!flushBody.isEmpty() && !flushBody.contains("break;"),
              "完结前补发：单格口补发失败不中断整轮（循环继续，不影响后续 H8）");
    }

    // ⑮ ★ 2026-09-25 现场口径更正：**解锁不发送满箱回传（H7）——只有锁格才发送**
    //    原实现：解锁时若该格仍有未上传记录（锁格时波次非执行态被跳过），会按当前绑定补发一次 H7；
    //    现场要求取消 —— 解锁只刷新状态 + 留痕提示，绝不产生回传报文（避免重复发送、
    //    以及把报文挂到"当前绑定/非本波次"的箱号上）。遗留记录由锁格/一键满箱/手动满箱/完结补发承担。
    {
        const QString unlockBody = functionBody(cpp, "&PlcManager::gridUnlocked", 60);
        const QString lockBody2  = functionBody(cpp, "&PlcManager::gridLocked", 90);
        check(!unlockBody.isEmpty() && lockBody2.contains("sendFullbox(grid)"),
              "锁格链路仍是满箱回传（H7）的触发点（sendFullbox(grid)）");
        check(!unlockBody.contains("sendFullbox"),
              "解锁链路**不再**调用 sendFullbox —— 解锁不发送满箱回传",
              QString("unlockBody 含 sendFullbox=%1").arg(unlockBody.contains("sendFullbox") ? 1 : 0));
        check(cpp.count("sendFullbox(grid)") == 1,
              "自动满箱回传只有锁格触发：sendFullbox(grid) 全源码唯一调用点",
              QString("occurrences=%1").arg(cpp.count("sendFullbox(grid)")));
        check(unlockBody.contains("解锁不发送满箱回传") && unlockBody.contains("pendingRecords"),
              "解锁时仍有未上传记录 ⇒ 只留痕提示（写明可用补传入口），记录保留在内存不丢");
        check(!unlockBody.contains("WAVE_SORTING") && !unlockBody.contains("WAVE_FULLBOX_SYNC"),
              "解锁不再按波次状态判定补发（原 st==SORTING/FULLBOX_SYNC → sendFullbox 分支已删除）");
    }

    // ⑯ ★ 2026-09-26 现场口径（B 方案）：**"H6 到场"≠"马上能收件"** ——
    //    PLC 仍处满箱锁格（S7 锁格位=1，换箱动作未结束）时，H6 **不立即**解除该格禁用，
    //    把"恢复收件"推迟到解锁边沿（下降沿）；解锁到达且该格已绑定容器时才恢复。
    //    现场实例：H6 08:26:55.023 到、锁格位 08:26:56.497 才复位 —— 原实现这 ≈1.5s 窗口内件照落 034
    //    （下发判据对"锁格"是照发：PlcManager::isGridDispatchable 第 2 行直接 return true）。
    {
        const QString h6Body     = functionBody(cpp, "QJsonObject HttpServer::handleBindingLatticePort", 200);
        const QString unlockBody = functionBody(cpp, "&PlcManager::gridUnlocked", 60);
        const QString plcCpp     = readSource(QDir::currentPath() + "/../WCS_httpServer/PlcManager.cpp");

        check(!h6Body.isEmpty() && h6Body.contains("isGridLocked(gridNum)"),
              "H6 绑定处先判 PLC 锁格位（满是锁格时不得直接恢复收件）");
        check(h6Body.indexOf("isGridLocked(gridNum)") < h6Body.indexOf("enableGrid(gridNum)"),
              "顺序：先判 isGridLocked → 未锁格才 enableGrid（恢复收件）",
              QString("locked@%1 enable@%2")
                  .arg(h6Body.indexOf("isGridLocked(gridNum)")).arg(h6Body.indexOf("enableGrid(gridNum)")));
        check(h6Body.contains("暂不恢复收件") && h6Body.contains("解锁后自动恢复收件"),
              QString::fromUtf8("锁格期间 H6 只留痕（日志 + 界面提示「暂不恢复收件，解锁后自动恢复」），不解除禁用"));

        check(!unlockBody.isEmpty() && unlockBody.contains("hasBoundContainer")
                  && unlockBody.contains("isGridDisabled"),
              QString::fromUtf8("解锁边沿：对「仍被禁用」的格口做恢复判定（H6 早到者的恢复点）"));
        check(unlockBody.contains("enableGrid") && unlockBody.contains("已绑定容器，格口已恢复收件"),
              QString::fromUtf8("解锁且已绑定容器 → enableGrid 恢复收件（面板橙→绿），并留痕/提示"));
        check(cpp.contains("isGridDisabled(gNum) && hasBoundContainer(gNum)"),
              QString::fromUtf8("恢复收件的判据 = 仍被禁用 且 已绑定容器（两者缺一不恢复）"));

        const QString reconcileBody = functionBody(cpp, "int HttpServer::reconcileStrandedDisabledGrids", 40);
        const QString healthBody    = functionBody(cpp, "void HttpServer::logHealthStatus", 200);
        check(hdr.contains("reconcileStrandedDisabledGrids") && healthBody.contains("reconcileStrandedDisabledGrids"),
              QString::fromUtf8("兜底对账挂在 10s 健康检查周期上（不依赖边沿信号，「掉线丢边沿」也能自愈）"));
        check(!reconcileBody.isEmpty()
                  && reconcileBody.contains("isGridDisabled") && reconcileBody.contains("isGridLocked")
                  && reconcileBody.contains("hasBoundContainer") && reconcileBody.contains("enableGrid"),
              QString::fromUtf8("对账判据三条齐备：仍被禁用 + 锁格位=0 + 已绑定容器 ⇒ enableGrid 恢复收件"));
        check(cpp.contains("[格口恢复] 兜底对账"),
              QString::fromUtf8("对账恢复留痕（http.log + 界面提示，可追溯）"));

        check(plcCpp.contains("已绑新箱待解锁"),
              QString::fromUtf8("选格日志单独计数「已绑新箱待解锁」(H6已绑新箱，等PLC解锁边沿)"
                                "（现场可区分：不是没容器，而是没解锁）"));
    }

    // ⑱ ★ 2026-09-26 现场口径（最终）：① 锁格一律不落件（含启动即锁格 / 锁格状态未知）② 未绑定容器
    //    一律不落件（开关废弃）③ 只有四种情形发 H7（结束任务 = 已绑定容器一键满箱；切回不自动补发）
    //    ④ 额度单元 = (SKU,格口,分拣类型)、不搬迁 ⑤ 异常口也严格 ⑥ 结束任务固定延迟 2 秒 + 无条件 H8。
    {
        const QString plcCpp = readSource(QDir::currentPath() + "/../WCS_httpServer/PlcManager.cpp");
        const QString plcHdr = readSource(QDir::currentPath() + "/../WCS_httpServer/PlcManager.h");
        const QString dblHdr = readSource(QDir::currentPath() + "/../WCS_httpServer/DoubleBuffer.h");
        const QString pwsCpp = readSource(QDir::currentPath() + "/../WCS_httpServer/ParseWorker.cpp");
        const QString wghHdr = readSource(QDir::currentPath() + "/../WCS_httpServer/WmsGridCode.h");
        const QString allocH  = readSource(QDir::currentPath() + "/../WCS_httpServer/PlanAllocTable.h");

        // ① 锁格不落件（判据 + 候选 + 兜底三处收口）
        const QString gate = functionBody(plcCpp, "bool PlcManager::isGridDispatchable", 20);
        check(gate.contains("isLockStateKnown()") && gate.contains("isGridLocked(grid)")
                  && gate.contains("return false"),
              "判据：锁格（含锁格状态未知）→ 不可下发");
        check(!plcCpp.contains("可用格口全部物理锁格→取首个匹配")
                  && !plcCpp.contains("单格口映射[%1]物理锁格中→按需求照发"),
              "已删除全部\u201c锁格照发\u201d兜底（多格口取首个匹配 / 单格口物理锁格中照发）");
        check(plcHdr.contains("m_lockStateKnown") && plcCpp.contains("m_lockStateKnown.store(true)"),
              "锁格状态就绪标志：首次轮询快照后置位（此前按\u201c锁格未知=不可下发\u201d保守处理）");
        check(plcCpp.contains("if (!isLockStateKnown() || isGridLocked(g)) { ++nLocked; continue; }"),
              "候选过滤：锁格/未知一律不进候选");

        // ② 未绑定容器不落件（开关废弃）
        check(!plcCpp.contains("config().sortingRequireBoundGrid"),
              "开关 sortingRequireBoundGrid 已废弃：判据不再读它（恒要求已绑定容器）");
        const QString cfgCpp = readSource(QDir::currentPath() + "/../WCS_httpServer/ConfigManager.cpp");
        check(cfgCpp.contains("sortingRequireBoundGrid=false 已废弃"),
              "旧 XML 若写 false → 只打一行 WARN\u201c已废弃、不生效\u201d");

        // ③ 异常口也严格
        check(!plcCpp.contains("仍按策略发往该口（若PLC回报无格口/失败"),
              "异常口同样严格：不可下发（锁格/未绑定容器/禁用）时不再\u201c按策略发往该口\u201d");
        check(plcCpp.contains("if (avail.empty() || isGridLocked(avail[0]) || !isGridBound(avail[0]))"),
              "单格口兜底带空/锁格/未绑定三重防护（原实现存在空 vector 越界）");

        // ④ 额度单元 = (SKU,格口,类型)；不搬迁
        check(wghHdr.contains("makeCellKey") && wghHdr.contains("cellKeyGridOf") && wghHdr.contains("cellKeyTypeOf"),
              "复合键助手齐备（makeCellKey / cellKeyGridOf / cellKeyTypeOf）");
        check(dblHdr.contains("planQtyPerCell") && pwsCpp.contains("planQtyPerCell[cellKey] += gridNumber"),
              "解析期按**单元**累加计划（同格口两类型各自保额，不再被后一行覆盖/按格口合并）");
        check(allocH.contains("if (g.grid == lastGrid && g.type == lastType) continue;"),
              "分配表去重键 = (格口,类型)（不再按格口丢行）");
        check(allocH.contains("q.landed + q.reserv > c.planQtyH4") || allocH.contains("landed + q.reserv > c.planQtyH4"),
              "巡检 ⑧：每单元 已落+在途 ≤ H4 计划（跨单元借用额度当场暴露）");
        check(!allocH.contains("int moveGap(")
                  && !plcHdr.contains("typedef std::function<int(const QString& epc, qint16 fromGrid, qint16 toGrid)> PlcMoveGapCallback;")
                  && !cpp.contains("int HttpServer::moveAllocGap(const QString& sku")
                  && !hdr.contains("int  moveAllocGap(const QString& sku"),
              "搬迁调用链已整体删除（PlanAllocTable::moveGap / PlcMoveGapCallback / HttpServer::moveAllocGap）");

        // ⑤ 只有四种情形发 H7
        check(cpp.contains("reportFullboxAllBoundGrids") && mw.contains("reportFullboxAllBoundGrids"),
              "「一键满箱回传」与「结束任务」共用同一实现（reportFullboxAllBoundGrids）");
        check(functionBody(cpp, "int HttpServer::flushUnreportedFullboxes", 90).contains("leftoverDesc")
                  && cpp.contains("满箱回传未完成"),
              "未绑定容器但有未上传记录的格口：跳过 + 留痕（含格口号/条数 + 手输格口号补传指引）");
        check(cpp.contains("resendOutbox(orderCode, /*resendH7=*/false, /*resendH8=*/true)"),
              "切回波次：不再自动补发满箱回传（H7），仅补发完结回传（H8）");
        check(cpp.contains("已绑定容器逐个 H7") && cpp.contains("detachOnSuccess"),
              "结束任务补发 = 已绑定容器逐个 H7（成功者按完结口径清绑定）");

        // ⑥ 结束任务固定延迟 2 秒 + 无条件 H8
        check(def.contains("#define END_REPORT_DELAY_MS 2000")
                  && readSource(QDir::currentPath() + "/../release_WcsHttpServer/config/http_server.xml")
                         .contains("<endReportDelayMs>2000</endReportDelayMs>"),
              "固定延迟 = 2 秒（define.h 默认 + 现场 XML 一致）");
        check(functionBody(cpp, "void HttpServer::onEndDelayTimeout()", 120).contains("emitEndReportNow()")
                  && !functionBody(cpp, "void HttpServer::onEndDelayTimeout()", 120).contains("return;   // 仍有未成功"),
              "延迟到点后**无条件**发送 H8（H7 结果只留痕，不等待、不拦截）");
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("\n== ④ 源码契约：单元归属（同一 (SKU,格口) 多类型）+ 落格反馈驱动额度归还 ==\n");
    //   现场口径（2026-09-26 业务确认）：
    //     · 同一 (SKU,格口) 可以存在多种 grid_type ⇒ 单元归属必须精确，不得串属性；
    //     · 件没进计划格口（落 66 / PLC 报无格口·信息不全 / 落错格）⇒ 不计分拣、不消耗计划额度，
    //       额度归还**以 PLC 真实落格反馈为准**（不再等 30s 超时清扫）；
    //     · 领受范围：不改消耗口径、不改巡检、不改 H7/H8 报文。
    // ════════════════════════════════════════════════════════════════════
    {
        const QString allocH = readSource(root + "/WCS_httpServer/PlanAllocTable.h");
        const QString sdbCpp = readSource(root + "/WCS_httpServer/SortingDatabase.cpp");
        const QString sdbH   = readSource(root + "/WCS_httpServer/SortingDatabase.h");

        // ① 归属存根（迟到反馈记回原认领单元）
        check(allocH.contains("m_orphanClaims") && allocH.contains("kOrphanKeepMs") && allocH.contains("kOrphanCap"),
              "归属存根齐备（m_orphanClaims + 保留期 + 条数上限）");
        const QString relBody = functionBody(allocH, "bool releaseEpc(const QString& epc", 40);
        check(relBody.contains("stashOrphan") && relBody.contains("stashAttribution"),
              "释放认领时按 stashAttribution 决定是否落存根（超时/发送失败=true；件未进计划格口=false）");
        const QString commitBody = functionBody(allocH, "bool commitOnLanded(const QString& sku", 120);
        check(commitBody.contains("m_orphanClaims.constFind(epc)"),
              "落格归属顺序含 ①b 归属存根（迟到反馈记回原单元，不串属性）");
        check(commitBody.indexOf("m_landedEpcs.contains(dedupKey") < commitBody.indexOf("m_orphanClaims.constFind"),
              "顺序正确：②已登记去重 在 ①b存根 之前（否则重复反馈会二次记账）");
        check(commitBody.contains("if (!epc.isEmpty()) m_orphanClaims.remove(epc);"),
              "落格后消费存根（不残留、不无界增长）");
        check(commitBody.contains("unitGuessedOut") && commitBody.contains("gridTypeHint"),
              "无法判定归属时回传 unitGuessedOut；恢复路径按 gridTypeHint 精确归属");
        check(functionBody(allocH, "bool noteIssued(const QString& epc", 40).contains("m_orphanClaims.remove(epc)"),
              "新认领登记即作废旧存根（以最新一次下发为准）");
        check(allocH.contains("m_orphanClaims.clear();"),
              "存根随波次清空（clear()，不跨波次归属）");
        check(cpp.contains("pruneOrphanClaims") && cpp.contains("PlanAllocTable::kOrphanKeepMs"),
              "30s 巡检顺带剪枝存根（超期清理，防无界增长）");
        check(allocH.contains("landedTypeOfEpc"),
              "重复类落格（不走 commitOnLanded）可反查单元类型，落格明细类型不丢");

        // ② 恢复波次按单元编译
        check(allocH.contains("inputsFromCells"),
              "单元编译助手 inputsFromCells 齐备（恢复/测试共用同一口径）");
        const QString resumeBody = functionBody(cpp, "bool HttpServer::resumeUnfinishedWave", 700);
        check(resumeBody.contains("PlanAllocTable::inputsFromCells(e.planQtyPerCell"),
              "恢复波次按**单元**编译（不再合并同格口的分类/发货两行）");

        // ③ 反馈驱动即时归还（三分支收集 + 信号 + 主线程槽 + 在途守卫 + 不落存根）
        check(hdr.contains("void allocClaimReleaseRequested(const QStringList& epcs);"),
              "新增信号 allocClaimReleaseRequested（反馈线程 → 主线程）");
        check(hdr.contains("void releaseAllocClaimsOnMissedLanding(const QStringList& epcs);"),
              "新增主线程槽 releaseAllocClaimsOnMissedLanding");
        check(cpp.contains("&HttpServer::allocClaimReleaseRequested, this") && cpp.contains("Qt::QueuedConnection"),
              "信号以 QueuedConnection 接到主线程槽（分配表写操作主线程独占）");
        const QString slotBody = functionBody(cpp, "void HttpServer::releaseAllocClaimsOnMissedLanding", 60);
        check(slotBody.contains("isEpcInFlight(epc)"),
              "守卫：该 EPC 已被重新下发时不释放新认领（防误放行重投件）");
        check(slotBody.contains("/*stashAttribution=*/false"),
              "三类\u201c件未进计划格口\u201d释放不留存根（避免把无据反馈记错属性）");
        check(cpp.contains("missedPlanLandingEpcs << e.code;"),
              "反馈侧收集\u201c件未进计划格口\u201d的 EPC（异常口/PLC无格口·信息不全/落错格）");
        check(cpp.indexOf("emit epcsLanded(") < cpp.indexOf("emit allocClaimReleaseRequested(missedPlanLandingEpcs)"),
              "发出顺序正确：先 epcsLanded（清在途标志）→ 再 allocClaimReleaseRequested（归还额度）");
        check(!cpp.contains("void HttpServer::releaseAlloc(const QString& epc)"),
              "零调用的死代码 releaseAlloc() 已删除（避免两种释放口径并存）");

        // ④ 落格单元类型持久化 + 恢复逐件归属
        check(def.contains("SQL_ALTER_SORTING_ADD_GRID_TYPE"),
              "define.h 提供旧库补列语句（grid_type）");
        check(sdbCpp.contains("q.exec(SQL_ALTER_SORTING_ADD_GRID_TYPE);"),
              "createTables 执行迁移（旧库自动补列）");
        check(sdbH.contains("const QString& gridType = QString()"),
              "insertRecord 新增单元类型参数（默认值，既有调用点不改）");
        check(sdbCpp.contains("gridType.isEmpty() ? QString(\"\") : gridType"),
              "空类型绑 ''（NOT NULL 列不能绑 null QString，否则 INSERT 静默失败）");
        check(cpp.contains("(landedType <= 2) ? QString::number((int)landedType) : QString()"),
              "落格时把**本次记账单元类型**写入明细（切回/断电重建按类型精确归属）");
        check(functionBody(cpp, "void HttpServer::restoreLandedProgress", 140).contains("r.gridType")
                  && functionBody(cpp, "void HttpServer::restoreLandedProgress", 140).contains(", hint, &landedType)"),
              "切回重建按明细里的 grid_type 精确归属（旧数据无该列 → 启发式 + unitGuessed 留痕）");
    }

    std::printf("\n==== 结果：PASS=%d FAIL=%d ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
