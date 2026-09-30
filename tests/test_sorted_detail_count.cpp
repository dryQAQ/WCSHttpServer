// ============================================================================
// test_sorted_detail_count.cpp — ★ 2026-09-25 现场反馈（可执行自测，非产品代码）
//
// 现场问题：「为什么在历史记录上看到的已分拣数量总是会减少几件？」
//
// 根因（**口径不一致，不是丢数据**）：
//   · 波次面板「分拣件数」原取 WaveManager::sorted() = PLC 反馈**件次**累计：
//     每次 PLC 报成功落格 +1（含重复反馈/重投），且含"只计件但没写落格明细"的四类件
//     —— 落格时格口无容器绑定(no_bind)、识别码无匹配(no_match)、落错格、超计划超出件；
//   · 「波次数据历史记录」页「已分拣」列取 sorting_records 的 COUNT(DISTINCT barcode)
//     —— 只算**真正写进落格明细**的件。
//   ⇒ 面板 ≥ 历史列，差额恰好是"只计件不写明细"的那几件。
//     现场库实测（release_WcsHttpServer/data/sorting_records.db，只读核对）：
//       波次 789（09-17）历史列 168，同期 http.log 有 19 条 `PLC反馈格口无绑定 … 计为已分拣`
//       → 当时面板 187，差额 19 件全部是 no_bind；全库 no_bind 共 448 条（09-09 起）。
//
// 本次统一口径（现场确认）：两边都用**去重实物件数**
//   = 真正写进落格明细(sorting_records) 的去重 EPC 数
//   = 面板 HttpServer::sortedDetailCount()（内存 m_lastDetailGridByEpc，实时、零 DB 查询）
//   = 历史列 COUNT(DISTINCT barcode)（DB）
//   ★ 该值同时就是 H7 满箱明细 / 箱内实落 / 上报 WMS 的件数口径。
//
// 本测试锁定两件事：
//   ① 数据层契约（真实 SortingDatabase = 历史列表取值来源）：
//      · 同一 EPC 的重复落格行（09-14 前历史库的遗留写法）、同一 EPC 落**两个格口**，
//        历史列都只算 1 件（与 (格口,EPC) 明细去重、跨格口补记并存不矛盾）；
//      · "只计件不写明细"的 no_bind / no_match 异常行**不会**让历史列变大
//        —— 差额由此产生，且属设计口径（这些件没进任何箱子、不进 H7）。
//   ② 源码契约（防回归，读产品源码断言）：
//      · HttpServer 提供 sortedDetailCount()，取值 = m_lastDetailGridByEpc.size()，
//        且该哈希是 **EPC 单键**（⇒ 同一件落 2 个格口仍算 1 件，与 COUNT(DISTINCT barcode) 同口径）；
//      · 落格明细去重集合的两个写入点都在（实际落格写明细 / 切回波次按 DB 明细重建）；
//      · 波次面板「分拣件数」标签取 sortedDetailCount()，**不得**再取快照的 sumLocation
//        （该字段是件次口径，正是本次现场问题的来源）；
//      · 历史列的 SQL 仍是 COUNT(DISTINCT s.barcode)（两侧口径同源的前提）。
//
// 构建：见 tests\run_tests.bat 第 [14/14] 步
// ============================================================================

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QDateTime>
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

// ── 造数：一条落格明细（与 HttpServer 落格写入口径一致：每条 = 1 件）──
static void addLanding(SortingDatabase& db, const QString& order, const QString& epc,
                       const QString& sku, const QString& grid, const QString& box)
{
    db.insertRecord(order, epc, sku, grid, "C1", "C1", "C1", 1, "--", box);
}

static void addException(SortingDatabase& db, const QString& order, const QString& type,
                         const QString& epc)
{
    ExceptionRecord e;
    e.type      = type;
    e.orderCode = order;
    e.epc       = epc;
    e.sku       = "SKU1";
    e.reason    = "自测造数";
    e.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    db.insertException(e);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString dbPath = QCoreApplication::applicationDirPath() + "/test_sorted_detail.db";
    QFile::remove(dbPath);
    QFile::remove(dbPath + "-wal");
    QFile::remove(dbPath + "-shm");

    SortingDatabase& db = SortingDatabase::instance();
    std::printf("== ① 打开数据库（自动建表）==\n");
    check(db.open(dbPath), QString("数据库打开成功"), dbPath);

    // ── 造数：一个波次，3 件真实落格 + 遗留重复行 + 跨格口同件 + 2 件"只计件不写明细"──
    const QString W = "TEST-SORTED-DETAIL";
    db.upsertReturnWave(W, 10, 3 /* WAVE_SORTING */);

    addLanding(db, W, "EPC-1", "SKU1", "001", "BOX1");   // 第 1 件
    addLanding(db, W, "EPC-2", "SKU1", "001", "BOX1");   // 第 2 件（同格口同箱）
    addLanding(db, W, "EPC-3", "SKU2", "002", "BOX2");   // 第 3 件
    addLanding(db, W, "EPC-1", "SKU1", "001", "BOX1");   // 09-14 前的遗留写法：同一 (格口,EPC) 重复行
    addLanding(db, W, "EPC-3", "SKU2", "003", "BOX3");   // 重扫落到**不同格口** → 允许补记第 2 条

    // 这两件 PLC 报成功（面板件次会 +1），但**没有写落格明细**（现场 187 vs 168 的差额来源）
    addException(db, W, "no_bind",  "EPC-9");            // 落格时格口无容器绑定
    addException(db, W, "no_match", "EPC-10");           // 识别码无匹配（09-09 口径后计已分拣）

    std::printf("== ② 历史列取值：sorting_records 去重 EPC（面板已与之对齐）==\n");
    QVector<WaveRecordProgress> waves = db.getAllWaves();
    WaveRecordProgress rec;
    bool found = false;
    for (const WaveRecordProgress& w : waves)
        if (w.orderCode == W) { rec = w; found = true; break; }

    check(found, QString("历史列表能查回该波次"));
    check(rec.sortedCount == 3,
          QString("历史列「已分拣」= 3（5 行明细里的 3 个不重复 EPC：遗留重复行不虚增、同一件跨格口仍算 1 件）"),
          QString::number(rec.sortedCount));

    check(db.queryByOrderCode(W).size() == 5,
          QString("底层明细确有 5 行（重复行 + 跨格口补记都照实留库，H7 按容器聚合用）"),
          QString::number(db.queryByOrderCode(W).size()));

    check(db.getSortedEpcsByOrder(W).size() == 3,
          QString("恢复/防重用的已分拣 EPC 集合同样 = 3（与历史列同口径）"),
          QString::number(db.getSortedEpcsByOrder(W).size()));

    // ★ 差额成因断言：no_bind/no_match 只写异常表、不写落格明细
    //   ⇒ 面板件次（PLC 报成功即 +1）会比历史列多出这几件，这正是现场"少几件"的全部来源。
    check(rec.sortedCount == 3,
          QString("no_bind/no_match 异常行**不**进历史列（2 件只在异常表留痕）→ 差额由此产生，属设计口径"),
          QString::number(rec.sortedCount));

    std::printf("== ③ 源码契约（防回归）==\n");
    const QString srcDir = QDir::currentPath() + "/../WCS_httpServer";
    const QString hSrv = readSource(srcDir + "/HttpServer.h");
    const QString cSrv = readSource(srcDir + "/HttpServer.cpp");
    const QString mw   = readSource(srcDir + "/MainWindow.cpp");
    const QString def  = readSource(srcDir + "/define.h");

    check(!hSrv.isEmpty() && !cSrv.isEmpty() && !mw.isEmpty() && !def.isEmpty(),
          QString("产品源码可读（HttpServer.h/.cpp、MainWindow.cpp、define.h）"));

    // ① 声明 + 实现
    check(hSrv.contains("int sortedDetailCount() const;"),
          QString("HttpServer.h 声明 sortedDetailCount()（去重实物件数）"));

    const QString body = functionBody(cSrv, "int HttpServer::sortedDetailCount() const");
    check(!body.isEmpty(), QString("HttpServer.cpp 实现 sortedDetailCount()"));
    check(body.contains("m_lastDetailGridByEpc.size()"),
          QString("取值 = m_lastDetailGridByEpc.size()（写进过落格明细的去重 EPC 数）"));

    // ② 该哈希必须是 EPC 单键 —— 否则"同一件落 2 个格口"会被算成 2 件，与历史列 DISTINCT 口径不符
    check(hSrv.contains("QHash<QString, QString> m_lastDetailGridByEpc;"),
          QString("m_lastDetailGridByEpc 是 EPC 单键哈希（同件跨格口只算 1 件，与 COUNT(DISTINCT barcode) 同口径）"));

    // ③ 两个写入点都在：实际落格写明细 + 切回波次按 DB 明细重建
    check(cSrv.count("markLandingDetailRecorded(") >= 3,
          QString("markLandingDetailRecorded 的写入点齐备（声明 + 落格写入 + 切回重建）"),
          QString::number(cSrv.count("markLandingDetailRecorded(")));

    // ④ 面板标签必须取新口径，且**不得**再取快照的 sumLocation（件次口径 = 本次现场问题来源）
    check(mw.contains("m_lblSumLocation->setText(QString::number(m_pServer->sortedDetailCount()))"),
          QString("波次面板「分拣件数」标签取 sortedDetailCount()（与历史列同源）"));
    check(!mw.contains("snap.sumLocation"),
          QString("MainWindow.cpp 已不再读快照的 sumLocation 字段（件次口径，禁用）"));

    // ⑤ 历史列 SQL 仍是 DISTINCT barcode —— 两侧同源的前提
    check(def.contains("COUNT(DISTINCT s.barcode) FROM sorting_records"),
          QString("历史列 SQL 仍为 COUNT(DISTINCT s.barcode)（面板与历史的共同口径）"));

    std::printf("\n==== 结果: %d 通过 / %d 失败 ====\n", g_pass, g_fail);
    db.close();
    return g_fail == 0 ? 0 : 1;
}
