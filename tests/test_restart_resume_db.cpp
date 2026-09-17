// ============================================================================
// test_restart_resume_db.cpp — ★ 2026-09-15 需求验证（可执行自测，非产品代码）
//
// 需求（客户口径）：
//   ① 每次关闭会**切出该波次任务**，保留所有信息以及格口绑定状态信息到数据库便于回溯；
//      每次开启软件 = **新任务状态**（自动开始新任务、新任务开始时清空格口绑定关系）。
//   ② 需要继续上次任务时：从「波次数据记录」切回该波次 → 进度与格口绑定一并恢复。
//   ③ ★★ 历史数据**不得被粘贴/复制进其它波次任务**，格口绑定状态尤其如此 ★★
//
// 本测试在数据库层锁定这条链路所依赖的**契约**（HttpServer 的分支逻辑不便单测，故测其输入）：
//   ① 关闭切出：绑定归档(active=0 + unbind_time)，行**不删除** → 仍可按波次回溯
//   ② 启动新任务：一律不把 DB 绑定装载到界面；残留 active 行一律归档（新任务无绑定）
//   ③ 切回波次：按该波次自己的记录恢复绑定（①级）+ 只读兜底；**任何一级都不写库**
//   ④ 进度恢复输入齐备：已落格明细、已分拣集合、未闭环异常、SKU→格口计划
//   ⑤ 正常「结束任务」后（终态 + 已归档）→ 切回也恢复不出绑定
//   ⑥ ★ 波次隔离不变量：多次切换/关闭重启后，每个波次的绑定行**只减不增**
//      （归档只改 active/unbind_time）→ 历史绝不被复制进别的波次
//   ⑦ ★ 不得复活历史旧箱：库里存有 65 个历史旧绑定（H-T0001..H-T0065）时，
//      切回只显示该波次自己的/当前活跃的绑定，不把旧箱绑到别的波次
//   ⑧ ★ 写入唯一来源 = H6：切回/沿用/启动兜底三条路径都不产生新绑定行
//   ★ 2026-09-16 新增（断电路径）：
//   ⑨ 断电（异常关闭，不走 closeEvent）在分拣中 → 绑定行仍 active=1 → 开机兜底归档 →
//      切回**逐格一致**（该绑的已绑、未绑的还是未绑，不串其它波次；反复断电/切回幂等）
//   ⑩ 断电后"继续分拣"的依据：切回时按 sorting_records 重建已落格进度
//      （H7 明细只补当前容器件 / 计划额度按整波次累计 / 落格去重集合齐全）
//   ⑪ H6 归属落空（内存无波次）→ 绑定行以 order_code='' **保留**，不再整行插入失败
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
                    got.isEmpty() ? "" : ("  ← 实际: " + got).toUtf8().constData());
    }
}

// ────────────────────────────────────────────────────────────────────────────
// 以下函数复刻 HttpServer 的判定口径（测 DB 契约，不依赖 GUI/网络）
// ────────────────────────────────────────────────────────────────────────────

// 启动口径（对应 HttpServer::restoreWaveFromDB）：
//   启动 = 新任务状态 → 一律不装载绑定；把 DB 中残留的 active 行全部归档。
//   返回本次启动归档的 active 行数（= 被"清空"的绑定数量）。
static int policyStartupArchive(SortingDatabase& db)
{
    const QVector<GridBoxBindRecord> actives = db.getAllActiveBinds();
    if (!actives.isEmpty())
        db.archiveAllBinds();
    return actives.size();
}

// 切出口径（对应 HttpServer::switchOutWaveForExit / switchAwayCurrentWave）：
//   只归档（active=0 + unbind_time 留痕），**不新增任何行**。
static void policySwitchOut(SortingDatabase& db)
{
    db.archiveAllBinds();
}

// 切回口径（对应 HttpServer::resumeUnfinishedWave 步骤6）：
//   ★★ 2026-09-17 口径修正（现场取证，见 docs/格口绑定波次归属_根因与修复_20260917.md）★★
//     主口径 = **本波次自己的记录**（getBindsByOrder：每格取"本波次内"最后一条，箱号非空）
//     兜底   = DB 中 active=1 的绑定（仅显示，非本波次记录）
//   ★ 为什么不再拿 getBindsByOrderActive 当主口径：
//     ① 它要求"该格全表最后一条属本波次"，实测真实库 14 个历史波次里 13 个返回 0 行；
//     ② 它一旦部分命中，旧实现整体替换内存绑定 → 其余格口全变"未绑定"（现场"切回后被清空"）。
//     ③ 数学上它 ⊂ getBindsByOrder → 先取后者绝不会漏格口。
//   ★ 全部只读：切回不写库（"历史不得粘贴进其它波次"铁律）。
//   ★ 终态波次（已完结/已取消）：产品侧入口守卫直接拒绝切回 → 不恢复绑定。
static QMap<QString, QString> policyRestoreAtSwitch(SortingDatabase& db, const QString& orderCode,
                                                    QString* sourceUsed = nullptr)
{
    if (sourceUsed) sourceUsed->clear();

    // 终态波次：不恢复绑定（产品侧同口径）
    const int st = db.getWaveStatus(orderCode);
    if (st == 8 /*WAVE_FINISHED*/ || st == 6 /*WAVE_CANCELLED*/)
        return QMap<QString, QString>();

    // 主口径：本波次自己的记录（每格取本波次内最后一条）
    const QMap<QString, QString> own = db.getBindsByOrder(orderCode);
    if (!own.isEmpty()) { if (sourceUsed) *sourceUsed = "wave"; return own; }

    // 兜底：DB 当前活跃绑定（只读显示，非本波次记录）
    QMap<QString, QString> act;
    for (const GridBoxBindRecord& b : db.getAllActiveBinds())
        if (!b.gridNum.isEmpty() && !b.boxcode.isEmpty())
            act.insert(b.gridNum, b.boxcode);
    if (!act.isEmpty()) { if (sourceUsed) *sourceUsed = "active"; return act; }

    if (sourceUsed) *sourceUsed = "none";
    return QMap<QString, QString>();
}

// ★ 2026-09-17 「新任务」清空格口绑定口径（对应 HttpServer::startNewWaveTask）：
//   现场要求"开始新任务时应清空格口绑定" → 面板全部回到「未绑定」。
//   但**不能**把"已到达、尚未归属（order_code=''）"的行一起归档：
//   否则随后 H4 到达时归属补齐不到它们 → 新波次又是 0 绑定（切回无绑定可恢复的老问题）。
//   返回本次归档的行数（= 被清空的已归属绑定数）。
static int policyNewTaskClearBinds(SortingDatabase& db)
{
    const QVector<GridBoxBindRecord> actives = db.getAllActiveBinds();
    int attributed = 0;
    for (const GridBoxBindRecord& b : actives)
        if (!b.orderCode.isEmpty()) ++attributed;
    db.archiveAttributedBinds();
    return attributed;
}

// ★ 2026-09-17 归属补齐/纠偏口径（对应 HttpServer::attributePendingBindsToWave
//   → SortingDatabase::attributeBindsToWave；判定全在 DB 侧，测试只喂入会话/窗口边界）：
//     sessionStart   = 本次会话启动时刻：早于它的行**永不改判**（历史行）
//     switchOutWave  = 切出窗口归属的波次号（可空）
//     switchOutTime  = 切出时刻（可空）：只有窗口内产生的行才允许从 switchOutWave 纠偏
static int policyAttributeBinds(SortingDatabase& db, const QString& orderCode,
                               const QString& sessionStart,
                               const QString& switchOutWave = QString(),
                               const QString& switchOutTime = QString())
{
    return db.attributeBindsToWave(orderCode, sessionStart, switchOutWave, switchOutTime);
}

// 会话开始时刻辅助：offsetSec<0 = 过去（"本会话内"的行），>0 = 未来（把已有行判为"历史行"）
static QString sessionTime(int offsetSec)
{
    return QDateTime::currentDateTime().addSecs(offsetSec).toString("yyyy-MM-dd HH:mm:ss.zzz");
}

// 波次隔离不变量辅助：某波次的绑定行数（含归档行）——用于断言"只减不增"
static int bindsRowCount(SortingDatabase& db, const QString& orderCode)
{
    return db.getBindHistoryByOrder(orderCode).size();
}

// ────────────────────────────────────────────────────────────────────────────
// 切回时"已落格进度"重建口径（对应 HttpServer::restoreLandedProgress）
//   数据源：sorting_records（getLandingRecordsForWave，按落格先后正序）= 唯一持久权威
//   重建三处（与实时落格路径逐条同口径）：
//     ① H7 明细分录：**只补"仍属于当前绑定容器"的件**（旧箱件已在旧箱 H7 里报过，不重复上报）
//     ② 计划额度/封顶计数：用**整波次全部落格**（跨容器累计，换箱不重置），键=(格口,SKU,EPC)
//     ③ 落格明细去重集合：**全部落格件**都登记，键=(格口,EPC)
// ────────────────────────────────────────────────────────────────────────────
struct LandedRebuildResult
{
    QMap<QString, int> detailPerGrid;   // ① 补入 H7 明细的件数（格口 → 件数）
    QSet<QString>      quotaKeys;       // ② 计划额度登记键 (格口,SKU,EPC)
    QSet<QString>      dedupKeys;       // ③ 落格去重登记键 (格口,EPC)
    int                oldBoxSkipped = 0;   // 换箱前旧容器件（不补明细）
    int                noBoxSkipped  = 0;   // 当前无绑定容器（无从归属，不补明细）
};

static QString padGridKey(const QString& grid)
{
    bool ok = false;
    const int g = grid.trimmed().toInt(&ok);
    return ok ? QString("%1").arg(g, 3, 10, QChar('0')) : grid.trimmed();
}

static LandedRebuildResult policyRebuildLanded(SortingDatabase& db, const QString& orderCode,
                                               const QMap<QString, QString>& currentBinds)
{
    LandedRebuildResult rr;
    const QVector<LandedRecord> landed = db.getLandingRecordsForWave(orderCode);
    for (const LandedRecord& r : landed)
    {
        const QString epc = r.epc.trimmed();
        if (epc.isEmpty()) continue;
        const QString gridKey = padGridKey(r.gridNum);
        if (gridKey.isEmpty()) continue;
        const QString sku = r.sku.trimmed();

        // ② 计划额度：跨容器累计（含旧箱件），同一件只计 1 次
        if (!sku.isEmpty())
            rr.quotaKeys.insert(gridKey + "\n" + sku + "\n" + epc);

        // ③ 去重集合：同一 (格口,EPC) 只登记一次；已登记过 → 不再补明细（口径同实时落格）
        const QString dedupKey = gridKey + "\n" + epc;
        const bool firstTime = !rr.dedupKeys.contains(dedupKey);
        rr.dedupKeys.insert(dedupKey);

        // ① H7 明细：只补当前仍绑定容器里的件（缺 SKU / 无绑定容器 / 旧容器 都不补）
        if (!firstTime || sku.isEmpty()) continue;
        const QString curBox = currentBinds.value(gridKey);
        if (curBox.isEmpty())                { ++rr.noBoxSkipped;  continue; }
        if (r.boxcode.trimmed() != curBox)   { ++rr.oldBoxSkipped; continue; }
        rr.detailPerGrid[gridKey] += 1;
    }
    return rr;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString dbPath = QCoreApplication::applicationDirPath() + "/test_restart_resume.db";
    QFile::remove(dbPath);
    QFile::remove(dbPath + "-wal");
    QFile::remove(dbPath + "-shm");

    SortingDatabase& db = SortingDatabase::instance();
    std::printf("== ① 打开数据库 ==\n");
    check(db.open(dbPath), QString("数据库打开成功"), dbPath);

    const QString WAVE = "PP2026RESUME01";        // 分拣中、格口已绑定好的波次
    const QString BOX1 = "H-T0101";               // 当前容器
    const QString BOX0 = "H-T0000";               // 换箱前的旧容器（其件已上报过，不应重复进 H7）

    // ── 造数：波次(分拣中) + 3 个格口绑定 + 计划明细 + 已落格 + 异常 ──
    db.upsertReturnWave(WAVE, 10, 3 /*WAVE_SORTING*/);
    db.bindGridBox("001", BOX1, WAVE);
    db.bindGridBox("002", BOX1, WAVE);
    db.bindGridBox("003", BOX1, WAVE);

    // 计划明细：SKU1 → 格口001*2 + 002*1（同品多格口）
    {
        QVector<ReturnWaveItemRecord> items;
        auto mk = [&](const QString& sku, const QString& grid, const QString& type, int qty) {
            ReturnWaveItemRecord it;
            it.orderCode = WAVE; it.inco = sku; it.gridNum = grid; it.gridType = type;
            it.planQty = qty; it.volu = "H-01-AB"; it.obxCode = BOX1;
            items.append(it);
        };
        mk("SKU1", "001", "0", 2);
        mk("SKU1", "002", "0", 1);
        mk("SKU2", "002", "0", 3);
        db.insertWaveItems(WAVE, items);
    }

    // 已落格明细：5 件
    //   · EPC-A1/A2/B1 在当前容器 BOX1 → 切回时应补入 H7 明细
    //   · EPC-OLD / EPC-X2 在旧容器 BOX0 → 属换箱前已上报件 / 已闭环件，不进当前箱明细
    const QStringList epcs  = {"EPC-A1", "EPC-A2", "EPC-B1", "EPC-OLD", "EPC-X2"};
    const QStringList skus  = {"SKU1",    "SKU1",    "SKU2",    "SKU1",     "SKU1"};
    const QStringList grids = {"001",     "001",     "002",     "001",      "003"};
    const QStringList boxes = {BOX1,      BOX1,      BOX1,      BOX0,       BOX0};
    for (int i = 0; i < epcs.size(); ++i)
    {
        db.insertRecord(WAVE, epcs[i], skus[i], grids[i], QString("0%1").arg(i + 1),
                        QString("0%1").arg(i + 1), QString("0%1").arg(i + 2), 1, "H-01-AB", boxes[i]);
    }
    // 异常留痕：EPC-X1 未闭环（无成功落格）；EPC-X2 已闭环（之后成功落格 → 上表已有其落格行 + handled=1）
    {
        ExceptionRecord e1; e1.type = "plc_no_grid"; e1.orderCode = WAVE; e1.epc = "EPC-X1";
        e1.sku = "SKU1"; e1.reason = "自测造数"; e1.time = "2026-09-15 10:00:00";
        db.insertException(e1);
        ExceptionRecord e2 = e1; e2.epc = "EPC-X2"; db.insertException(e2);
        db.markExceptionResolved(WAVE, "EPC-X2");
    }

    std::printf("== ② 关闭切出口径：绑定归档(active=0) + 解绑时间留痕，行不删除 ==\n");
    {
        check(db.getAllActiveBinds().size() == 3,
              QString("关闭前：3 个格口处于已绑定（active=1）"),
              QString::number(db.getAllActiveBinds().size()));

        policySwitchOut(db);   // switchOutWaveForExit 的绑定归档动作

        check(db.getAllActiveBinds().isEmpty(),
              QString("归档后 active 归零 = 新任务无绑定"),
              QString::number(db.getAllActiveBinds().size()));

        // 行未删除 + 归属/时间留痕（回溯依据）
        const QVector<GridBoxBindRecord> hist = db.getBindHistoryByOrder(WAVE);
        check(hist.size() == 3, QString("绑定历史 3 条仍保留（行不删除）"),
              QString::number(hist.size()));
        bool allArchivedWithTime = !hist.isEmpty();
        QStringList desc;
        for (const GridBoxBindRecord& b : hist)
        {
            if (b.active || b.unbindTime.isEmpty() || b.boxcode != BOX1 || b.orderCode != WAVE)
                allArchivedWithTime = false;
            desc << QString("%1→%2(%3)").arg(b.gridNum, b.boxcode, b.unbindTime);
        }
        check(allArchivedWithTime,
              QString("每条均带容器号/解绑时间/波次归属（可回溯）：%1").arg(desc.join(",")),
              desc.join(","));
        check(db.getBindsByOrder(WAVE).size() == 3,
              QString("按波次快照仍可回查 3 条（追溯口径）"),
              QString::number(db.getBindsByOrder(WAVE).size()));
    }

    std::printf("== ③ 开启软件：新任务状态（不装载任何绑定）==\n");
    {
        // 模拟"残留 active"：旧波次的遗留行，以及异常关闭未归档的当前波次行
        db.bindGridBox("005", "H-T0909", "PP2025OLD");   // 旧库遗留（另一个波次）
        db.bindGridBox("006", BOX1,       WAVE);         // 本次未结束波次的残留（异常关闭场景）
        const int archived = policyStartupArchive(db);
        check(archived == 2, QString("启动把残留 active 2 个归档（新任务开始时清空格口绑定关系）"),
              QString::number(archived));
        check(db.getAllActiveBinds().isEmpty(), QString("启动后 DB 无 active 绑定 = 界面全为未绑定"),
              QString::number(db.getAllActiveBinds().size()));
        check(db.getLatestUnfinishedWave().orderCode == WAVE,
              QString("未结束波次仍可检出（提示「可切回继续」）"),
              db.getLatestUnfinishedWave().orderCode);
        check(db.getBindHistoryByOrder(WAVE).size() == 4,
              QString("本波次历史绑定 4 条（001/002/003/006）完整保留"),
              QString::number(db.getBindHistoryByOrder(WAVE).size()));
    }

    std::printf("== ④ 切回该波次：绑定随波次恢复 + 进度输入齐备 ==\n");
    {
        // 现场会有别波次的绑定：造一个"更晚波次"的绑定，验证不会串到本波次
        const QString WOTHER = "PP2026OTHER";
        db.upsertReturnWave(WOTHER, 3, 3);
        db.bindGridBox("008", "H-TOTHER", WOTHER);
        policySwitchOut(db);   // 该波次也切出

        QString src = "";
        const QMap<QString, QString> binds = policyRestoreAtSwitch(db, WAVE, &src);
        check(binds.size() == 4 && binds.value("001") == BOX1 && binds.value("002") == BOX1
                  && binds.value("003") == BOX1 && binds.value("006") == BOX1 && src == "wave",
              QString("切回恢复 4 个绑定且容器号正确（来源=%1：本波次自己的记录）").arg(src),
              QString("count=%1 src=%2").arg(binds.size()).arg(src));
        check(!binds.contains("008") && !binds.contains("005"),
              QString("不串入其它波次的绑定（005 属 PP2025OLD、008 属 %1）").arg(WOTHER),
              binds.contains("008") ? binds.value("008") : QString("-"));

        const QVector<LandedRecord> landed = db.getLandingRecordsForWave(WAVE);
        check(landed.size() == 5, QString("已落格明细 5 件"), QString::number(landed.size()));
        check(landed.size() >= 1 && landed.first().epc == "EPC-A1" && landed.first().gridNum == "001",
              QString("明细按落格先后正序，首件 EPC-A1 @格口001"),
              landed.isEmpty() ? QString() : landed.first().epc + "@" + landed.first().gridNum);
        check(landed.size() >= 4 && landed[2].sku == "SKU2" && landed[2].boxcode == BOX1
                  && landed[2].volu == "H-01-AB" && landed[2].timeMs > 0,
              QString("明细带 SKU/容器号/库位/时间（H7 报文与超计划判定所需）"),
              landed.size() >= 4 ? QString("%1/%2/%3/%4").arg(landed[2].sku, landed[2].boxcode,
                                                              landed[2].volu).arg(landed[2].timeMs)
                                 : QString());
        check(landed.size() >= 4 && landed[3].boxcode == BOX0,
              QString("旧容器件保留原容器号（切回时按当前容器过滤，不重复上报）"),
              landed.size() >= 4 ? landed[3].boxcode : QString());

        check(db.getSortedEpcsByOrder(WAVE).size() == 5, QString("已分拣 EPC 集合 5 件（进度恢复）"),
              QString::number(db.getSortedEpcsByOrder(WAVE).size()));
        check(db.getExceptionEpcsByOrder(WAVE).size() == 1,
              QString("未闭环异常 EPC 1 件（口径=无成功落格记录：EPC-X2 已落格闭环故不计）"),
              QString::number(db.getExceptionEpcsByOrder(WAVE).size()));
        check(db.getWaveItems(WAVE).size() == 3, QString("SKU→格口计划明细 3 行（映射恢复）"),
              QString::number(db.getWaveItems(WAVE).size()));
        check(db.getReturnWave(WAVE).status == 3,
              QString("波次状态仍为分拣中（切出不改写状态，切回即继续）"),
              QString::number(db.getReturnWave(WAVE).status));
    }

    std::printf("== ⑤ 正常「结束任务」后：终态 + 已归档 → 切回也恢复不出绑定 ==\n");
    {
        policySwitchOut(db);
        db.updateWaveStatus(WAVE, 8 /*WAVE_FINISHED*/);

        check(db.getAllActiveBinds().isEmpty(),
              QString("无 active 绑定可恢复"),
              QString::number(db.getAllActiveBinds().size()));
        check(policyRestoreAtSwitch(db, WAVE).isEmpty(),
              QString("正常结束后切回：恢复不出任何绑定（界面保持未绑定）"),
              QString::number(policyRestoreAtSwitch(db, WAVE).size()));
        check(db.getLatestUnfinishedWave().orderCode != WAVE,
              QString("已完结波次不再算未结束"),
              db.getLatestUnfinishedWave().orderCode);
        check(db.getBindHistoryByOrder(WAVE).size() == 4,
              QString("历史绑定行仍保留（可回溯，仅 active=0）"),
              QString::number(db.getBindHistoryByOrder(WAVE).size()));
    }

    std::printf("== ⑥ 新波次：绑定 → 关闭切出 → 启动新任务 → 切回恢复 ==\n");
    {
        const QString W2 = "PP2026RESUME02";
        db.upsertReturnWave(W2, 4, 3);
        db.bindGridBox("009", "H-T0202", W2);
        policySwitchOut(db);                      // 关闭切出

        const int archived = policyStartupArchive(db);
        check(archived == 0, QString("启动：已无残留 active（上次关闭已归档）"),
              QString::number(archived));
        check(db.getAllActiveBinds().isEmpty(), QString("启动后界面全为未绑定"));

        QString src = "";
        const QMap<QString, QString> binds = policyRestoreAtSwitch(db, W2, &src);
        check(binds.size() == 1 && binds.value("009") == "H-T0202" && src == "wave",
              QString("切回新波次：恢复 1 个绑定（来源=%1）").arg(src),
              QString("count=%1 src=%2").arg(binds.size()).arg(src));
    }

    std::printf("== ⑦ 【现场关键】切换波次任务：切走再切回，绑定必须完整恢复 ==\n");
    {
        const QString WA = "PP2026SWITCH_A";
        const QString WB = "PP2026SWITCH_B";
        db.upsertReturnWave(WA, 6, 3 /*分拣中*/);
        db.upsertReturnWave(WB, 6, 3);
        db.bindGridBox("010", "H-A-10", WA);
        db.bindGridBox("011", "H-A-11", WA);

        policySwitchOut(db);                          // 切到 B 前先切出 A（归档 A 的绑定）
        db.bindGridBox("020", "H-B-20", WB);          // B 有自己的绑定（WMS 重发 H6）

        QString srcA = "", srcB = "";
        const QMap<QString, QString> backA = policyRestoreAtSwitch(db, WA, &srcA);
        check(backA.size() == 2 && backA.value("010") == "H-A-10" && backA.value("011") == "H-A-11"
                  && srcA == "wave",
              QString("切回 A：绑定完整恢复（%1 个，来源=%2）").arg(backA.size()).arg(srcA),
              QString("count=%1 src=%2").arg(backA.size()).arg(srcA));
        check(!backA.contains("020"), QString("切回 A 不串入 B 的绑定（020 属 %1）").arg(WB),
              backA.contains("020") ? backA.value("020") : QString("-"));

        const QMap<QString, QString> backB = policyRestoreAtSwitch(db, WB, &srcB);
        check(backB.size() == 1 && backB.value("020") == "H-B-20" && srcB == "wave",
              QString("再切回 B：绑定完整恢复（来源=%1）").arg(srcB),
              QString("count=%1 src=%2").arg(backB.size()).arg(srcB));

        const QMap<QString, QString> backA2 = policyRestoreAtSwitch(db, WA, &srcA);
        check(backA2.size() == 2 && srcA == "wave",
              QString("A↔B 来回切换多次后，A 仍按自己的记录恢复（来源=%1）").arg(srcA),
              QString("count=%1 src=%2").arg(backA2.size()).arg(srcA));
    }

    std::printf("== ⑧ 【铁律】历史不得被粘贴进其它波次：切换多轮后各行数只减不增 ==\n");
    {
        const QString WA = "PP2026SWITCH_A";
        const QString WB = "PP2026SWITCH_B";
        const int a0 = bindsRowCount(db, WA);   // A 的行数（切换前）
        const int b0 = bindsRowCount(db, WB);

        // 反复"切走 → 切回"多轮（模拟现场来回切换 + 关闭重启）
        for (int round = 0; round < 3; ++round)
        {
            policySwitchOut(db);                                   // 切出（归档）
            policyRestoreAtSwitch(db, WA);                         // 切回 A（只读恢复，不得写库）
            policySwitchOut(db);
            policyRestoreAtSwitch(db, WB);                         // 切回 B
            policyStartupArchive(db);                              // 关闭软件 + 重启兜底
        }

        check(bindsRowCount(db, WA) == a0,
              QString("A 的绑定行数不变（%1 → %2）：切回只读，不复制、不新增")
                  .arg(a0).arg(bindsRowCount(db, WA)),
              QString::number(bindsRowCount(db, WA)));
        check(bindsRowCount(db, WB) == b0,
              QString("B 的绑定行数不变（%1 → %2）").arg(b0).arg(bindsRowCount(db, WB)),
              QString::number(bindsRowCount(db, WB)));

        // 每个波次只能看到自己的格口（历史不串波次）
        const QMap<QString, QString> aBinds = db.getBindsByOrder(WA);
        const QMap<QString, QString> bBinds = db.getBindsByOrder(WB);
        check(!aBinds.contains("020") && !bBinds.contains("010") && !bBinds.contains("011"),
              QString("A/B 的绑定互不掺入（A=%1 条、B=%2 条）").arg(aBinds.size()).arg(bBinds.size()),
              QString("A020=%1 B010=%2").arg(aBinds.contains("020") ? 1 : 0)
                  .arg(bBinds.contains("010") ? 1 : 0));

        // 从未绑定过的波次：切回也不得凭空多出绑定记录
        const QString WNOBIND = "PP2026NOBIND";
        db.upsertReturnWave(WNOBIND, 4, 3);
        policyRestoreAtSwitch(db, WNOBIND);
        check(db.getBindsByOrder(WNOBIND).isEmpty() && db.getBindsByOrderActive(WNOBIND).isEmpty(),
              QString("从未绑定过的波次：切回后仍无绑定记录（不臆造、不写库）"),
              QString::number(db.getBindsByOrder(WNOBIND).size()));
    }

    std::printf("== ⑨ 【现场回归】不得复活历史旧箱（H-T0001..H-T0065）==\n");
    {
        // 复现现场现象：库里存有 65 个"很早以前用过、当前并非在用"的绑定，
        //   而当前现场实际只绑了 2 个格口。
        const QString WOLD = "PP2026LEGACY_OLD";    // 历史波次（早已完结）
        const QString WNEW = "PP2026LEGACY_NEW";    // 当前波次
        db.upsertReturnWave(WOLD, 4, 8 /*已完结*/);
        db.upsertReturnWave(WNEW, 4, 3);
        for (int g = 1; g <= 65; ++g)
        {
            const QString grid = QString("%1").arg(g, 3, 10, QChar('0'));
            const QString box  = QString("H-T%1").arg(g, 4, 10, QChar('0'));
            db.bindGridBox(grid, box, WOLD);
        }
        policySwitchOut(db);   // 历史全部归档 → 当前无活跃绑定

        // 当前现场真正在用的是 002 / 066 两个格口
        db.bindGridBox("002", "H-REAL-002", WNEW);
        db.bindGridBox("066", "H-REAL-066", WNEW);

        // ① 切回新波次：只恢复本波次自己的 2 个，历史旧箱一个都不带
        QString src = "";
        const QMap<QString, QString> r = policyRestoreAtSwitch(db, WNEW, &src);
        bool hasLegacy = false;
        for (auto it = r.constBegin(); it != r.constEnd(); ++it)
            if (it.value().startsWith("H-T")) { hasLegacy = true; break; }
        check(r.size() == 2 && r.value("002") == "H-REAL-002" && r.value("066") == "H-REAL-066"
                  && !hasLegacy && src == "wave",
              QString("切回当前波次：只恢复自己的 2 个绑定，历史旧箱 H-T00xx 一个都不带（来源=%1）").arg(src),
              QString("count=%1 legacy=%2").arg(r.size()).arg(hasLegacy ? 1 : 0));

        // ② 新波次无绑定记录且现场无活跃绑定 → 保持未绑定、不写库
        const QString WFRESH = "PP2026LEGACY_FRESH";
        db.upsertReturnWave(WFRESH, 4, 1);
        policySwitchOut(db);   // 现场当前无活跃绑定
        const QMap<QString, QString> r2 = policyRestoreAtSwitch(db, WFRESH, &src);
        check(r2.isEmpty() && src == "none" && db.getBindsByOrder(WFRESH).isEmpty(),
              QString("无可用绑定时：保持未绑定、不写库（来源=%1）").arg(src),
              QString("count=%1 src=%2").arg(r2.size()).arg(src));

        // ③ 历史波次行数不变；新波次仍为 0 行（历史没有被粘贴过来）
        check(bindsRowCount(db, WFRESH) == 0 && bindsRowCount(db, WOLD) == 65,
              QString("历史波次行数不变（%1），新波次仍为 0 行").arg(bindsRowCount(db, WOLD)),
              QString("WFRESH=%1 WOLD=%2").arg(bindsRowCount(db, WFRESH)).arg(bindsRowCount(db, WOLD)));
    }

    std::printf("== ⑩ 写入唯一来源 = H6：切回/沿用/启动兜底都不产生新行 ==\n");
    {
        const QString WX = "PP2026WRITEONLY_H6";
        const QString WY = "PP2026OTHER_WAVE";
        db.upsertReturnWave(WX, 4, 3);
        db.upsertReturnWave(WY, 4, 3);
        db.bindGridBox("030", "H-WY-30", WY);      // 只有 H6 会写入（此处直接调 DB 模拟 H6）

        const int wy0 = bindsRowCount(db, WY);
        policySwitchOut(db);
        policyRestoreAtSwitch(db, WX);             // WX 无自己的记录 → 走兜底（只读）
        policyRestoreAtSwitch(db, WY);
        policyStartupArchive(db);
        policyRestoreAtSwitch(db, WX);

        check(bindsRowCount(db, WX) == 0,
              QString("WX 始终没有绑定行（切回/兜底都不写库）"),
              QString::number(bindsRowCount(db, WX)));
        check(bindsRowCount(db, WY) == wy0,
              QString("WY 的绑定行数不变（%1）——历史未被复制进 WX").arg(wy0),
              QString::number(bindsRowCount(db, WY)));

        QString src = "";
        const QMap<QString, QString> shown = policyRestoreAtSwitch(db, WX, &src);
        check(src == "snapshot" || src == "active" || src == "none",
              QString("WX 无自己的记录 → 走只读兜底（来源=%1，显示 %2 个，未写库）").arg(src).arg(shown.size()),
              QString("src=%1").arg(src));
    }

    // ══════════════════════════════════════════════════════════════════════════
    // ★ 2026-09-16 现场需求①：终态（已完成/已取消）波次**不允许切回**
    //   锁定两条 DB 层契约：
    //     ① 终态判定：status ∈ {6(已取消), 8(已完成)} → 不可切回；
    //        非终态（1/2/3/4/5/7/9）→ 可切回；
    //     ② 「拒绝切回」必须是**零副作用**：调用前后该波次的绑定行数、active 位完全不变
    //        （UI/HttpServer 的守卫都放在"切出当前波次"之前，这里用行数/active 快照锁住）。
    // ══════════════════════════════════════════════════════════════════════════
    std::printf("== ⑪ 【需求①】终态波次不允许切回：判定 + 零副作用 ==\n");
    {
        const QString WDONE  = "PP2026DONE";
        const QString WCANC  = "PP2026CANCEL";
        const QString WSORT  = "PP2026STILLSORTING";

        // 三个波次都绑定过容器（终态波次同样带历史绑定行，用于验证"拒绝时不改动它们"）
        db.upsertReturnWave(WDONE, 5, 8 /*WAVE_FINISHED*/);
        db.upsertReturnWave(WCANC, 5, 6 /*WAVE_CANCELLED*/);
        db.upsertReturnWave(WSORT, 5, 3 /*WAVE_SORTING*/);
        db.bindGridBox("031", "H-DONE-31", WDONE);
        db.bindGridBox("031", "H-DONE-31b", WDONE);   // 同格换箱一次（该行被覆盖，仍属本波次）
        db.bindGridBox("032", "H-CANC-32", WCANC);
        db.bindGridBox("033", "H-SORT-33", WSORT);

        // ① 终态判定
        check(db.getWaveStatus(WDONE) == 8 && db.getWaveStatus(WCANC) == 6,
              QString("终态波次状态正确（已完成=8 / 已取消=6）"),
              QString("done=%1 cancel=%2").arg(db.getWaveStatus(WDONE)).arg(db.getWaveStatus(WCANC)));
        check(db.getWaveStatus(WSORT) == 3,
              QString("非终态波次（分拣中=3）仍可切回"),
              QString::number(db.getWaveStatus(WSORT)));

        // ② 拒绝切回 = 零副作用：快照前后一致
        auto activeCount = [&](const QString& orderCode) {
            int n = 0;
            for (const GridBoxBindRecord& b : db.getBindHistoryByOrder(orderCode))
                if (b.active) ++n;
            return n;
        };
        const int doneRows0 = bindsRowCount(db, WDONE),  doneAct0 = activeCount(WDONE);
        const int cancRows0 = bindsRowCount(db, WCANC),  cancAct0 = activeCount(WCANC);

        // 复刻 HttpServer::resumeUnfinishedWave 的入口守卫：终态 → 直接拒绝，不做任何后续动作
        auto policyTerminalResume = [&](const QString& orderCode) -> bool {
            const int st = db.getWaveStatus(orderCode);
            if (st == 8 || st == 6) return false;   // 拒绝：不切换、不动绑定、不动内存
            return true;                            // 允许：继续后续切回流程
        };
        check(!policyTerminalResume(WDONE),  QString("已完成波次被拒绝切回"));
        check(!policyTerminalResume(WCANC),  QString("已取消波次被拒绝切回"));
        check(policyTerminalResume(WSORT) ,  QString("分拣中波次允许切回"));

        check(bindsRowCount(db, WDONE) == doneRows0 && activeCount(WDONE) == doneAct0,
              QString("拒绝切回「已完成」波次：绑定行数与 active 位完全不变（零副作用）"),
              QString("rows=%1→%2 act=%3→%4").arg(doneRows0).arg(bindsRowCount(db, WDONE))
                    .arg(doneAct0).arg(activeCount(WDONE)));
        check(bindsRowCount(db, WCANC) == cancRows0 && activeCount(WCANC) == cancAct0,
              QString("拒绝切回「已取消」波次：绑定行数与 active 位完全不变（零副作用）"),
              QString("rows=%1→%2 act=%3→%4").arg(cancRows0).arg(bindsRowCount(db, WCANC))
                    .arg(cancAct0).arg(activeCount(WCANC)));

        // ③ 终态波次的历史绑定仍可**只读回溯**（拒绝切回 ≠ 删除数据）
        check(bindsRowCount(db, WDONE) >= 2,
              QString("终态波次的绑定历史仍完整保留（可只读回溯，%1 行）").arg(bindsRowCount(db, WDONE)),
              QString::number(bindsRowCount(db, WDONE)));
    }

    // ══════════════════════════════════════════════════════════════════════════
    // ★ 2026-09-16 断电路径（设备断电 = 异常关闭，**不走** closeEvent/switchOutWaveForExit）
    //   现场情景：任务下发 → 格口容器全部绑定 → 点「开始分拣」(分拣中) → 设备断电 →
    //             次日开机（软件自启动 = 新任务状态）→ 历史波次数据里看到昨晚波次 →
    //             切回该波次 → 仍可继续分拣，且**绑定状态完好（该绑的已绑、未绑的还是未绑）**。
    //   与"正常关闭"的唯一差别：断电时绑定行仍是 active=1（内存里的 m_containerBindings
    //   随进程消失），由下次启动的 restoreWaveFromDB() 兜底归档；波次状态保持原值。
    // ══════════════════════════════════════════════════════════════════════════
    std::printf("== ⑫ 【断电路径】分拣中已绑定 → 断电(未归档) → 开机新任务 → 切回逐格一致 ==\n");
    {
        policySwitchOut(db);   // 上一轮会话正常关闭（现场历史：所有 active 行已归档）

        const QString WP = "PP2026POWERLOSS";
        db.upsertReturnWave(WP, 12, 3 /*WAVE_SORTING 分拣中*/);
        db.bindGridBox("040", "H-P-040",  WP);
        db.bindGridBox("041", "H-P-041a", WP);
        db.bindGridBox("041", "H-P-041b", WP);   // 同格换箱：最后一条才是"当前容器"
        db.bindGridBox("042", "H-P-042",  WP);
        // 043/066 等其余格口在本波次**从未绑定**（"未绑的还是未绑"的对照）

        check(db.getAllActiveBinds().size() == 3,
              QString("断电瞬间：库里 3 行仍为 active=1（异常关闭不走关闭切出；041 换箱的旧行已归档）"),
              QString::number(db.getAllActiveBinds().size()));
        check(bindsRowCount(db, WP) == 4,
              QString("该波次绑定历史 4 行（040/041 换箱前后两行/042，可逐条回溯）"),
              QString::number(bindsRowCount(db, WP)));

        // ── 次日开机：新任务状态（restoreWaveFromDB = 清界面 + 归档 DB 残留 active）──
        const int archived = policyStartupArchive(db);
        check(archived == 3, QString("开机把断电残留的 3 个 active 归档（新任务状态：界面全未绑定）"),
              QString::number(archived));
        check(db.getAllActiveBinds().isEmpty(), QString("开机后 DB 无 active 绑定 = 面板全为未绑定"),
              QString::number(db.getAllActiveBinds().size()));
        check(db.getLatestUnfinishedWave().orderCode == WP,
              QString("历史波次数据里能看到昨晚的波次（未结束 → 提示可切回）"),
              db.getLatestUnfinishedWave().orderCode);
        check(db.getWaveStatus(WP) == 3,
              QString("断电不改写波次状态：仍是分拣中(3)"),
              QString::number(db.getWaveStatus(WP)));
        check(!(db.getWaveStatus(WP) == 6 || db.getWaveStatus(WP) == 8),
              QString("分拣中 → 非终态 → 允许切回（终态守卫不拦它）"));

        // ── 切回昨晚波次：逐格一致 ──
        QString src = "";
        const QMap<QString, QString> binds = policyRestoreAtSwitch(db, WP, &src);
        check(binds.size() == 3 && binds.value("040") == "H-P-040"
                  && binds.value("041") == "H-P-041b" && binds.value("042") == "H-P-042"
                  && src == "wave",
              QString("切回：该绑的已绑（3 个，含换箱后的当前箱）来源=%1").arg(src),
              QString("count=%1 src=%2").arg(binds.size()).arg(src));
        check(!binds.contains("043") && !binds.contains("066") && !binds.contains("003")
                  && !binds.contains("030"),
              QString("切回：未绑的仍是未绑（不凭空补绑定、不串其它波次）"),
              QString("043=%1 066=%2").arg(binds.contains("043") ? 1 : 0)
                    .arg(binds.contains("066") ? 1 : 0));

        // ── 再断电一次 + 再开机 + 再切回：结果完全一致（幂等，绑定行数只减不增）──
        const int rowsBefore = bindsRowCount(db, WP);
        policyStartupArchive(db);                          // 断电（无归档）→ 开机兜底归档
        const QMap<QString, QString> again = policyRestoreAtSwitch(db, WP, &src);
        check(again == binds && bindsRowCount(db, WP) == rowsBefore,
              QString("反复断电/切回后绑定仍逐格一致（%1 行不变：切回只读、不复制）").arg(rowsBefore),
              QString("count=%1 rows=%2").arg(again.size()).arg(bindsRowCount(db, WP)));
    }

    std::printf("== ⑬ 【断电后继续分拣】切回时按 DB 重建已落格进度（H7明细/计划额度/去重）==\n");
    {
        const QString WP = "PP2026POWERLOSS";
        // 断电前已落格（写进 sorting_records，= 重启后唯一的持久权威）：
        //   040 当前箱 H-P-040 两件；041 当前箱 H-P-041b 一件；
        //   042 旧箱 H-P-042-OLD 一件（换箱前已上报过）；043 未绑定容器一件。
        //   另加一条**重复行**（同 (格口,EPC) 历史重复写入）→ 必须只算 1 件。
        struct L { const char* epc; const char* sku; const char* grid; const char* box; };
        const L rows[] = {
            { "EPC-P1",  "SKU-P1", "040", "H-P-040"     },
            { "EPC-P2",  "SKU-P1", "040", "H-P-040"     },
            { "EPC-P3",  "SKU-P2", "041", "H-P-041b"    },
            { "EPC-P4",  "SKU-P1", "042", "H-P-042-OLD" },
            { "EPC-P5",  "SKU-P1", "043", "H-P-043"     },
            { "EPC-P1",  "SKU-P1", "040", "H-P-040"     },   // 历史重复行（09-14 之前的写法）
        };
        for (const L& r : rows)
            db.insertRecord(WP, r.epc, r.sku, r.grid, "01", "01", "02", 1, "H-01-AB", r.box);

        const QMap<QString, QString> binds = policyRestoreAtSwitch(db, WP);   // 切回（先恢复绑定）
        const LandedRebuildResult rr = policyRebuildLanded(db, WP, binds);

        check(binds.value("040") == "H-P-040" && binds.value("041") == "H-P-041b",
              QString("重建前绑定已恢复（明细过滤要用当前容器号）"),
              QString("040=%1 041=%2").arg(binds.value("040"), binds.value("041")));
        check(rr.detailPerGrid.value("040") == 2 && rr.detailPerGrid.value("041") == 1
                  && !rr.detailPerGrid.contains("042") && !rr.detailPerGrid.contains("043"),
              QString("① H7 明细只补当前容器内的件（040:2 / 041:1；旧箱件与无绑定格口不进）"),
              QString("040=%1 041=%2 042=%3").arg(rr.detailPerGrid.value("040"))
                    .arg(rr.detailPerGrid.value("041")).arg(rr.detailPerGrid.value("042")));
        check(rr.oldBoxSkipped == 1 && rr.noBoxSkipped == 1,
              QString("① 跳过明细：换箱前旧容器 1 件 + 当前无绑定容器 1 件（不重复上报）"),
              QString("old=%1 nobox=%2").arg(rr.oldBoxSkipped).arg(rr.noBoxSkipped));
        check(rr.quotaKeys.size() == 5,
              QString("② 计划额度/封顶按**整波次全部落格**重建（跨容器累计 5 件，重复行只算 1 件）"),
              QString::number(rr.quotaKeys.size()));
        check(rr.quotaKeys.contains("042\nSKU-P1\nEPC-P4")
                  && rr.quotaKeys.contains("043\nSKU-P1\nEPC-P5"),
              QString("② 旧箱件/无绑定格口的件同样占额度（换箱不重置额度、不突破计划件数）"));
        check(rr.dedupKeys.size() == 5,
              QString("③ 落格明细去重集合含全部落格件（继续分拣时同一件不再多记一条）"),
              QString::number(rr.dedupKeys.size()));
        check(db.getSortedEpcsByOrder(WP).size() == 5,
              QString("已分拣 EPC 集合 5 件（进度输入齐备）"),
              QString::number(db.getSortedEpcsByOrder(WP).size()));
    }

    std::printf("== ⑭ 【断电场景相关】H6 归属落空：绑定行仍写入（order_code=''），不整行丢失 ==\n");
    {
        policySwitchOut(db);   // 清空 active（模拟开机前的状态）

        // 归属为空（内存无波次、且不在切出窗口内）→ 必须写入 order_code=''，**不能整行失败**
        //   历史缺陷：绑 NULL 触发 `NOT NULL constraint failed: grid_box_bind.order_code`
        //   → 现场表现为"面板显示已绑定、库里一行都没有"（断电后这行绑定就永久丢了）。
        check(db.bindGridBox("050", "H-NOWAVE-050", QString()),
              QString("归属落空的绑定写入成功（不再因 NOT NULL 整行失败）"));
        const GridBoxBindRecord cur = db.getActiveBind("050");
        check(cur.boxcode == "H-NOWAVE-050" && cur.orderCode.isEmpty(),
              QString("该行以 order_code='' 保留，作「当前活跃绑定」可查（不猜测归属）"),
              QString("box=%1 order=%2").arg(cur.boxcode, cur.orderCode));
        check(!db.getBindsByOrderActive("PP2026POWERLOSS").contains("050"),
              QString("切回任何波次都不会把「无归属」行算成该波次的绑定"),
              db.getBindsByOrderActive("PP2026POWERLOSS").value("050"));

        const int archived = policyStartupArchive(db);
        check(archived == 1 && db.getAllActiveBinds().isEmpty(),
              QString("开机兜底归档该行（新任务状态无 active 绑定）"),
              QString("archived=%1 active=%2").arg(archived).arg(db.getAllActiveBinds().size()));
        check(db.getActiveBind("050").boxcode.isEmpty(),
              QString("归档后该行不再活跃（行保留、只改 active/unbind_time，可回溯）"));
    }

    std::printf("== ⑮ 【归属补齐】H6 早于 H4：空归属行在 H4 到达时改判给该波次（现场波次 789 型）==\n");
    {
        const QString W = "PP2026H6BEFOREH4";
        db.upsertReturnWave(W, 66, 3);          // H4 到达（波次落库完成）
        policyStartupArchive(db);               // 开机：新任务状态（清空 active）

        // H6 早于 H4：内存无波次 → 归属写空串（不再是"整行丢失"）
        const QString sess = sessionTime(-120); // 本会话开始（早于下面写入的行 = 本会话内）
        db.bindGridBox("060", "H-BEF-60", QString());
        db.bindGridBox("061", "H-BEF-61", QString());
        check(db.getBindsByOrder(W).isEmpty(),
              QString("改判前：该波次无绑定记录（H6 归属落空）→ 现场表现就是「切回没绑定」"));

        // H4 到达 → 归属补齐
        const int changed = policyAttributeBinds(db, W, sess);
        check(changed == 2, QString("H4 到达时归属补齐改判 2 行"), QString::number(changed));

        const QMap<QString, QString> own = db.getBindsByOrder(W);
        check(own.size() == 2 && own.value("060") == "H-BEF-60" && own.value("061") == "H-BEF-61",
              QString("改判后该波次有了自己的绑定记录（2 格）"),
              QString("count=%1").arg(own.size()));
        QString src = "";
        const QMap<QString, QString> restored = policyRestoreAtSwitch(db, W, &src);
        check(restored == own && src == "wave",
              QString("切回该波次即可恢复这 2 个绑定（来源=本波次记录）来源=%1").arg(src),
              QString("count=%1 src=%2").arg(restored.size()).arg(src));

        // 归属补齐不新增/不删除行（只改 order_code）
        check(bindsRowCount(db, W) == 2,
              QString("归属补齐全过程行数不变（2 行，只改归属、不复制）"),
              QString::number(bindsRowCount(db, W)));
        check(db.attributeBindsToWave(W, sess, QString(), QString()) == 0,
              QString("重复补齐是幂等的（第二次改判 0 行）"));
    }

    std::printf("== ⑯ 【归属纠偏】切出窗口内的 H6 被记到上一个波次 → H4 到达时纠偏（现场 456456/6565656 型）==\n");
    {
        const QString WA = "PP2026WINDOUT_A";       // 刚被切出的波次（窗口归属目标）
        const QString WB = "PP2026WINDOUT_B";       // 紧接着下发的新波次（真正的主人）
        const QString HIST = "PP2026WINDOUT_HIST";  // 历史波次（其已归属行永不被改判）
        db.upsertReturnWave(WA, 12, 8);         // A 已完成（先它被切出）
        db.upsertReturnWave(WB, 1320, 3);       // B 随后下发
        db.upsertReturnWave(HIST, 12, 3);

        policyStartupArchive(db);

        // 会话/窗口边界：这些行是本会话内写的；switchOutTime 覆盖它们 → 属"窗口内误归属"
        const QString sess     = sessionTime(-60);
        const QString winStart = sessionTime(-30);

        // 已归属别的波次的行（不是空归属、也不是切出窗口的波次）→ 永不被改判
        db.bindGridBox("070", "H-OTHER-70", HIST);

        // 切出 A（窗口 armed）→ 窗口内到达的 H6：按现场口径先记到 A（三个格口都还没有 B 的记录）
        db.bindGridBox("071", "H-B-71", WA);
        db.bindGridBox("072", "H-B-72", WA);
        db.bindGridBox("073", "H-B-73", WA);
        const int aRows0 = bindsRowCount(db, WA);
        const int bRows0 = bindsRowCount(db, WB);

        check(db.getBindsByOrder(WB).isEmpty(),
              QString("H4(B) 到达前：B 无绑定记录（现场表现=切回 B 一条绑定都没有）"));

        // B 的 H4 到达 → 纠偏：窗口内、当前活跃、且 B 尚无该格记录的行整体改判给 B
        //   ★ 取舍说明（有意如此）：窗口内的行**无法从时间上区分**"是 A 的重发"还是"B 的投递"，
        //     而现场实测这两种情形里"属于即将下发的 B"占绝对多数（09-15 23:53 那 66 行全属 B）
        //     → 统一改判给紧随其后的 H4 波次；A 自己真正的历史行（窗口外/或 A 有记录）不受影响。
        const int changed = policyAttributeBinds(db, WB, sess, WA, winStart);
        check(changed == 3, QString("纠偏改判 3 行（窗口内的 071/072/073 归到 B）"), QString::number(changed));
        check(db.getBindsByOrder(WB).size() == 3
                  && db.getBindsByOrder(WB).value("071") == "H-B-71"
                  && db.getBindsByOrder(WB).value("073") == "H-B-73",
              QString("B 拿到本该属于它的绑定（3 格）"),
              QString("count=%1").arg(db.getBindsByOrder(WB).size()));
        check(db.getBindsByOrder(WA).isEmpty(),
              QString("A 名下不再留着被误挂的行（A 未被污染；该行数归零是改归属的正常结果）"),
              QString("A=%1").arg(db.getBindsByOrder(WA).size()));
        check(bindsRowCount(db, WA) + bindsRowCount(db, WB) == aRows0 + bRows0,
              QString("纠偏只在波次之间改归属：两波次行数之和不变（%1+%2 → 未新增/未删除行）")
                  .arg(aRows0).arg(bRows0),
              QString("A=%1 B=%2").arg(bindsRowCount(db, WA)).arg(bindsRowCount(db, WB)));

        // 边界①：已归属其它波次的行（既非空归属、也不是切出窗口的波次）永不改判
        check(db.getBindsByOrder(HIST).value("070") == "H-OTHER-70",
              QString("已归属其它波次的行不被改判（HIST 仍 070→H-OTHER-70）"),
              db.getBindsByOrder(HIST).value("070"));

        // 边界②：本波次已有该格记录 → 不抢（B 自己后来收到合法 H6）
        db.bindGridBox("074", "H-A-74", WA);          // 窗口内又来了一个 H6（暂挂 A）
        db.bindGridBox("074", "H-B-74-NEW", WB);      // B 自己的合法 H6（同格换箱：A 的行被归档）
        const int ch2 = policyAttributeBinds(db, WB, sess, WA, winStart);
        check(ch2 == 0 && db.getBindsByOrder(WB).value("074") == "H-B-74-NEW",
              QString("B 已有该格记录 → 不再改判（避免把已在用的记录改错）"),
              QString("changed=%1 074=%2").arg(ch2).arg(db.getBindsByOrder(WB).value("074")));

        // 边界③：会话之前的行（历史行）永不改判 —— 会话起点取未来时刻即等价于"所有行都是历史行"
        const QString sessFuture = sessionTime(60);
        const int histBefore = bindsRowCount(db, HIST);
        policyAttributeBinds(db, WB, sessFuture, QString(), QString());
        check(db.getBindsByOrder(HIST).value("070") == "H-OTHER-70"
                  && bindsRowCount(db, HIST) == histBefore,
              QString("会话之前的行不被改判（sessionStart 之后的才算本会话）"),
              db.getBindsByOrder(HIST).value("070"));
    }

    std::printf("== ⑰ 【取数不丢格口】部分命中不得整体替换（现场「切回后被清空」根因之一）==\n");
    {
        const QString W = "PP2026PARTIALHIT";
        db.upsertReturnWave(W, 66, 3);
        policyStartupArchive(db);

        // 本波次绑了 3 格；随后"更晚的波次"又把其中 2 格抢走：
        //   → getBindsByOrderActive(W) 只剩 1 格（部分命中），旧实现就只恢复这 1 格。
        db.bindGridBox("080", "H-P-80", W);
        db.bindGridBox("081", "H-P-81", W);
        db.bindGridBox("082", "H-P-82", W);
        db.bindGridBox("080", "H-LATER-80", "PP2026LATERWAVE");
        db.bindGridBox("081", "H-LATER-81", "PP2026LATERWAVE");

        const QMap<QString, QString> strictHit = db.getBindsByOrderActive(W);
        check(strictHit.size() == 1 && strictHit.value("082") == "H-P-82",
              QString("确认口径（每格全表最后一条属本波次）只剩 1 格 —— 正是旧实现整体替换的隐患"),
              QString("count=%1").arg(strictHit.size()));

        QString src = "";
        const QMap<QString, QString> restored = policyRestoreAtSwitch(db, W, &src);
        check(restored.size() == 3 && restored.value("080") == "H-P-80"
                  && restored.value("081") == "H-P-81" && restored.value("082") == "H-P-82"
                  && src == "wave",
              QString("切回恢复该波次**全部 3 格**（不再因部分命中而丢格口）来源=%1").arg(src),
              QString("count=%1 src=%2").arg(restored.size()).arg(src));
        check(!restored.contains("083"),
              QString("本波次从未绑过的格口仍为未绑定（不臆造）"));
    }

    std::printf("== ⑱ 【列表口径】绑定格口数批量统计 + 与切回取数/弹窗同口径 ==\n");
    {
        bool ok = false;
        const QMap<QString, int> counts = db.getBindCountsByOrder(&ok);
        check(ok, QString("批量统计查询成功（UI 波次列表「格口绑定」列的数据源）"));

        const QString W = "PP2026PARTIALHIT";
        check(counts.value(W, 0) == 3,
              QString("该波次统计 = 3 格（列表数字即「切回时将恢复的格口数」）"),
              QString::number(counts.value(W, 0)));
        check(!counts.contains("PP2026NEVERBOUND") && counts.value("PP2026NEVERBOUND", 0) == 0,
              QString("从未绑定过的波次不出现在结果中（列表显示「无绑定记录」）"));

        // 逐波次核对（与批量逐位一致）
        bool allMatch = true;
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
            if (db.getBindsByOrder(it.key()).size() != it.value()) { allMatch = false; break; }
        check(allMatch, QString("批量统计与逐波次取数逐位一致（无两套口径）"));

        // 「查看绑定」弹窗口径 = 切回取数口径（同一 SQL、带绑定/解绑时间）
        const QVector<GridBoxBindRecord> detail = db.getLastBindsByOrder(W);
        const QMap<QString, QString> restored = policyRestoreAtSwitch(db, W);
        check(detail.size() == restored.size(),
              QString("弹窗明细条数 = 切回恢复格口数（%1）").arg(restored.size()),
              QString("detail=%1 restore=%2").arg(detail.size()).arg(restored.size()));
        bool sameMap = (detail.size() == restored.size());
        for (const GridBoxBindRecord& r : detail)
            if (restored.value(r.gridNum) != r.boxcode) { sameMap = false; break; }
        check(sameMap, QString("弹窗明细逐格与切回恢复一致（同一口径，防漂移）"));
        bool hasTime = !detail.isEmpty();
        for (const GridBoxBindRecord& r : detail)
            if (r.bindTime.isEmpty()) { hasTime = false; break; }
        check(hasTime, QString("弹窗明细带绑定时间（可逐条回溯）"));
    }

    std::printf("== ⑲ 【新任务】清空格口绑定：面板复位为未绑定，但记录归档保留、待补齐行不丢 ==\n");
    {
        const QString WA = "PP2026NEWTASK_A";       // 当前波次（已绑定 3 格）
        const QString WB = "PP2026NEWTASK_B";       // 新任务后 WMS 下发的下一波次
        db.upsertReturnWave(WA, 12, 3);
        db.upsertReturnWave(WB, 1320, 3);
        policyStartupArchive(db);

        const QString sess = sessionTime(-60);
        // A 的绑定（已归属）：090/091/092
        db.bindGridBox("090", "H-A-090", WA);
        db.bindGridBox("091", "H-A-091", WA);
        db.bindGridBox("092", "H-A-092", WA);
        // 已到达但尚未归属的 H6（内存无波次 → order_code=''，等 H4 到达补齐）
        db.bindGridBox("093", "H-B-093", QString());

        check(db.getAllActiveBinds().size() == 4,
              QString("清空前：4 个活跃绑定（A 的 3 个 + 待补齐 1 个）"),
              QString::number(db.getAllActiveBinds().size()));

        // ── 点「新任务」：清空格口绑定 ──
        const int cleared = policyNewTaskClearBinds(db);
        check(cleared == 3, QString("新任务归档了 3 个**已归属**绑定（= 面板被清空的绑定数）"),
              QString::number(cleared));
        const QVector<GridBoxBindRecord> actAfter = db.getAllActiveBinds();
        check(actAfter.size() == 1 && actAfter.first().gridNum == "093"
                  && actAfter.first().orderCode.isEmpty(),
              QString("待补齐行（093，order_code=''）**保留 active**，不能被一起归档"),
              QString("active=%1").arg(actAfter.size()));

        // ① 该波次的绑定记录仍在库里（行与归属都没动）→ 切回时逐格恢复
        QString src = "";
        const QMap<QString, QString> backA = policyRestoreAtSwitch(db, WA, &src);
        check(backA.size() == 3 && backA.value("090") == "H-A-090"
                  && backA.value("091") == "H-A-091" && backA.value("092") == "H-A-092"
                  && src == "wave",
              QString("新任务后切回 A：绑定记录完好，逐格恢复到清空前（来源=%1）").arg(src),
              QString("count=%1 src=%2").arg(backA.size()).arg(src));

        // ② 归档行带解绑时间留痕（active=0 + unbind_time 有值），可逐条回溯
        bool allArchivedWithTime = true;
        for (const GridBoxBindRecord& r : db.getBindHistoryByOrder(WA))
            if (r.active || r.unbindTime.isEmpty()) { allArchivedWithTime = false; break; }
        check(allArchivedWithTime,
              QString("A 的绑定行已归档留痕（active=0 且 unbind_time 有值，行不删除）"));

        // ③ 新波次 B 的 H4 到达 → 待补齐行被补齐（新任务没有破坏归属补齐链路）
        const int attr = policyAttributeBinds(db, WB, sess);
        check(attr == 1 && db.getBindsByOrder(WB).value("093") == "H-B-093",
              QString("H4(B) 到达后待补齐行归到 B（新任务清空不破坏归属补齐）"),
              QString("changed=%1 093=%2").arg(attr).arg(db.getBindsByOrder(WB).value("093")));
        const QMap<QString, QString> backB = policyRestoreAtSwitch(db, WB, &src);
        check(backB.size() == 1 && backB.value("093") == "H-B-093" && src == "wave",
              QString("切回 B 可恢复其 1 个绑定（新任务 → H6 早于 H4 → H4 补齐 → 切回，全链路通）"),
              QString("count=%1 src=%2").arg(backB.size()).arg(src));
    }

    db.close();
    std::printf("\n===== 结果：通过 %d 项，失败 %d 项 =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
