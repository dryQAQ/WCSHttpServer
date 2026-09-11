#include "MainWindow.h"
#include "ConfigManager.h"
#include "LogService.h"
#include "SortingDatabase.h"
#include "WmsGridCode.h"   // ★ 2026-09-07 WMS 格口编码(22+3位)：手动满箱输入归一
#include "define.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QApplication>
#include <QDateTime>
#include <QCloseEvent>
#include <QMessageBox>
#include <QFrame>
#include <QScrollArea>
#include <QVector>
#include <QMap>            // ★ 2026-09-10 SKU查询：波次→计划格口分组
#include <QSet>            // ★ 2026-09-10 SKU查询：实际落格格口去重
#include <QStringList>     // ★ 2026-09-10 SKU查询：计划格口列表
#include <QFont>           // ★ 2026-09-10 实际落格号异常高亮
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextCursor>
#include <QDialog>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QXmlStreamReader>
#include <QFile>
#include <QSplitter>
#include <QShowEvent>
#include <QHideEvent>
#include "qcustomplot.h"   // ★ 2026-09-07 效率统计图（QCustomPlot）

// ============================================================================
// ★ 2026-09-08 UI调整 第四行实时滚动表公共逻辑：
//   新数据插到第 0 行（最新在最上，面板持续滚动不跳动），
//   超过 LIVE_TABLE_MAX_ROWS 行后自动裁掉最旧（末）行，控制内存与渲染量。
//   行数上限只影响"保留明细时长"，不影响性能：QTableView 只绘制可见行，
//   插头/删尾均为轻量操作（经现场实测 600~800ms/件 速率下占用可忽略）。
// ============================================================================
static const int LIVE_TABLE_MAX_ROWS = 5000;

// ============================================================================
// ★ 2026-09-10 查询更新：格口号归一（显示与匹配统一口径）
//   现场三种写法 → 统一内部 3 位 key（与 PLC 反馈 / 绑定 / DB 存储一致）：
//     "7"（裸数字） / "007"（补零） / "22007"（WMS 格口编码 22+3位） → "007"
//   ★ 2026-09-11：统一委托 WmsGridCode.h::normalizeGridKey()（全系统单一实现，
//     含"前缀未配置"兜底）；UI 侧只做调用，不做任何与输入写法相关的差异处理。
// ============================================================================
static QString gridKeyOf(const QString& gridStr)
{
    return normalizeGridKey(gridStr);
}

static void pushLiveRow(QTableWidget* tbl, const QStringList& cells, bool warnRed = false)
{
    if (!tbl) return;
    const int n = qMin(cells.size(), tbl->columnCount());
    if (n <= 0) return;
    if (tbl->rowCount() >= LIVE_TABLE_MAX_ROWS)
        tbl->removeRow(tbl->rowCount() - 1);   // 裁掉最旧行

    tbl->insertRow(0);                          // 最新插入最上
    for (int c = 0; c < n; ++c)
    {
        QTableWidgetItem* it = new QTableWidgetItem(cells.at(c));
        // 序号/时间居中，内容列左对齐；EPC 用等宽字体便于现场比对
        if (c == 2) { QFont f = it->font(); f.setFamily("Consolas"); it->setFont(f); }
        it->setTextAlignment((c == 0 || c == 1)
            ? int(Qt::AlignHCenter | Qt::AlignVCenter)
            : int(Qt::AlignLeft | Qt::AlignVCenter));
        if (warnRed) it->setForeground(QColor("#E53935"));   // 异常状态整行标红
        tbl->setItem(0, c, it);
    }
    tbl->scrollToTop();
}

// ============================================================================
// EfficiencyChartDialog — RFID 推送效率统计弹窗（2026-09-07）
//   · 图1（柱状）：最近 30 分钟窗口、1 分钟最小刻度 → 每分钟 RFID 推送件数
//   · 图2（折线）：今天 0 点~23 点 → 对应时刻峰值效率（件/时）
// 说明：独立弹窗，不参与主界面布局；仅"当日观察"统计，不留存记录；
//   弹窗打开期间 1s 刷新（复用绘图缓冲、1440 点规模，内存可控），关闭即停止
// ============================================================================
class EfficiencyChartDialog : public QDialog
{
public:
    EfficiencyChartDialog(HttpServer* srv, QWidget* parent = nullptr)
        : QDialog(parent), m_srv(srv)
    {
        setWindowTitle(QString::fromUtf8("RFID 推送效率统计（当日观察）"));
        resize(920, 660);

        QVBoxLayout* lay = new QVBoxLayout(this);

        // ── 图1：最近 30 分钟柱状（每分钟件数，取自主流程分桶）──
        auto* capMin = new QLabel(QString::fromUtf8("柱状图：最近 30 分钟 · 每分钟 RFID 推送件数"), this);
        capMin->setStyleSheet("font-size: 12px; font-weight: bold; color: #333;");
        lay->addWidget(capMin);
        m_plotMin = new QCustomPlot(this);
        m_plotMin->setMinimumHeight(250);
        lay->addWidget(m_plotMin);
        m_bars = new QCPBars(m_plotMin->xAxis, m_plotMin->yAxis);
        m_bars->setPen(Qt::NoPen);
        m_bars->setBrush(QColor("#2196F3"));
        m_plotMin->xAxis->setLabel(QString::fromUtf8("时间（最近 30 分钟，最小刻度 1 分钟）"));
        m_plotMin->yAxis->setLabel(QString());          // ★ y 轴仅数值显示
        m_plotMin->yAxis->setNumberFormat("f");
        m_plotMin->yAxis->setNumberPrecision(0);
        m_plotMin->xAxis->setRange(-0.6, 29.6);
        m_plotMin->yAxis->setRange(0, 10);
        m_plotMin->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        // ★ 2026-09-07 柱状图 x 轴 30 个分钟刻度文字重叠 → 斜向 60° 显示 + 小字号
        //   （QCP 2.1.1 自动外边距已计入斜向文字高度，无需手工留边）
        m_plotMin->xAxis->setTickLabelRotation(60);
        m_plotMin->xAxis->setTickLabelFont(QFont(font().family(), 8));

        // ── 图2：今日 0~23 时折线（每小时记 1 点 = 该小时峰值效率：小时峰值件数 ×60 件/时）──
        auto* capDay = new QLabel(QString::fromUtf8("折线图：今天 0 点~23 点 · 每小时峰值效率（件/时）"), this);
        capDay->setStyleSheet("font-size: 12px; font-weight: bold; color: #333;");
        lay->addWidget(capDay);
        m_plotDay = new QCustomPlot(this);
        m_plotDay->setMinimumHeight(250);
        lay->addWidget(m_plotDay);
        m_line = m_plotDay->addGraph();
        m_line->setPen(QPen(QColor("#FF9800"), 2));
        m_line->setAdaptiveSampling(true);   // ★ 大数据量自适应采样，降低渲染消耗
        m_line->setLineStyle(QCPGraph::lsLine);
        m_plotDay->xAxis->setLabel(QString::fromUtf8("时间（今天 0 点 ~ 23 点）"));
        m_plotDay->yAxis->setLabel(QString());          // ★ y 轴仅数值显示
        m_plotDay->yAxis->setNumberFormat("f");
        m_plotDay->yAxis->setNumberPrecision(0);
        m_plotDay->xAxis->setRange(-1.0, 24.0);   // x = 小时 0..23
        m_plotDay->yAxis->setRange(0, 10);
        m_plotDay->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

        // 标题行：日期 + 当前/峰值提示（轻量刷新，不重建控件）
        m_lblInfo = new QLabel(QString::fromUtf8("当前日期：%1").arg(QDate::currentDate().toString("yyyy-MM-dd")));
        m_lblInfo->setStyleSheet("font-size: 13px; font-weight: bold; color: #555;");
        lay->insertWidget(0, m_lblInfo);

        m_timer = new QTimer(this);
        m_timer->setInterval(1000);          // 弹窗打开期间 1s 实时刷新
        connect(m_timer, &QTimer::timeout, this, &EfficiencyChartDialog::refresh);
        refresh();
    }

    void showEvent(QShowEvent* ev) override { m_timer->start(); refresh(); QDialog::showEvent(ev); }
    void hideEvent(QHideEvent* ev) override { m_timer->stop(); QDialog::hideEvent(ev); }

private slots:
    void refresh()
    {
        if (!m_srv) return;
        QVector<int> lastMin;
        m_srv->efficiencySeries(30, &lastMin, nullptr);   // 柱状：主流程每分钟分桶件数
        const qint64 nowMin = QDateTime::currentMSecsSinceEpoch() / 60000;
        const QString today = QDate::currentDate().toString("yyyy-MM-dd");
        m_lblInfo->setText(QString::fromUtf8("日期：%1    最近1分钟 %2 件（%3 件/时）    当日峰值 %4 件/分（%5 件/时）")
            .arg(today)
            .arg(m_srv->rfidPushPerMinute())
            .arg(m_srv->rfidPushPerMinute() * 60)
            .arg(m_srv->peakPerMinuteToday())
            .arg(m_srv->peakPerMinuteToday() * 60));

        // ── 柱状：最近 30 个整分钟桶（旧→新），柱标签=该分钟时刻 HH:MM ──
        QVector<double> keys(lastMin.size()), vals(lastMin.size());
        QVector<double> tickPos;
        QVector<QString> tickLabels;
        for (int i = 0; i < lastMin.size(); ++i)
        {
            keys[i] = i;
            vals[i] = lastMin[i];
            const qint64 bucketStart = (nowMin - (lastMin.size() - 1 - i)) * 60000;
            tickPos.append(i);
            tickLabels << QDateTime::fromMSecsSinceEpoch(bucketStart).toString("HH:mm");
        }
        m_bars->setData(keys, vals);
        auto textTicker = QSharedPointer<QCPAxisTickerText>::create();
        textTicker->addTicks(tickPos, tickLabels);
        m_plotMin->xAxis->setTicker(textTicker);
        m_plotMin->xAxis->setRange(-0.6, qMax(lastMin.size() - 0.4, 0.4));
        // y 轴自适应 + 顶部留 15% 余量（0 值数据时保留基准 10）
        m_plotMin->yAxis->rescale(true);
        double yMax = m_plotMin->yAxis->range().upper;
        m_plotMin->yAxis->setRange(0, yMax > 0 ? yMax * 1.15 : 10.0);
        m_plotMin->replot(QCustomPlot::rpQueuedReplot);

        // ── 折线：今日每小时峰值效率（每小时记 1 点 = 该小时峰值件数 × 60）──
        QVector<int> hourPeaks;
        m_srv->efficiencySeries(30, nullptr, &hourPeaks);
        QVector<double> dKeys, dVals;
        dKeys.reserve(24);
        dVals.reserve(24);
        for (int h = 0; h < 24; ++h)
        {
            dKeys << h;                          // x = 小时 0..23
            dVals << hourPeaks[h] * 60.0;        // y = 该小时峰值效率（件/时）
        }
        m_line->setData(dKeys, dVals);
        QVector<double> dayPos;
        QVector<QString> dayTickLabels;
        for (int h = 0; h <= 23; ++h)
        {
            dayPos.append(h);
            dayTickLabels << QString("%1点").arg(h);
        }
        auto dayTicker = QSharedPointer<QCPAxisTickerText>::create();
        dayTicker->addTicks(dayPos, dayTickLabels);
        m_plotDay->xAxis->setTicker(dayTicker);
        m_plotDay->yAxis->rescale(true);
        double dMax = m_plotDay->yAxis->range().upper;
        m_plotDay->yAxis->setRange(0, dMax > 0 ? dMax * 1.15 : 10.0);
        m_plotDay->replot(QCustomPlot::rpQueuedReplot);
    }

private:
    HttpServer*      m_srv    = nullptr;
    QCustomPlot*     m_plotMin = nullptr;
    QCustomPlot*     m_plotDay = nullptr;
    QCPBars*         m_bars    = nullptr;
    QCPGraph*        m_line    = nullptr;
    QLabel*          m_lblInfo = nullptr;
    QTimer*          m_timer   = nullptr;
};

// ============================================================================
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("WCS退货HTTP服务 -- V1.0");
    resize(1280, 900);          // ★ 2026-09-08 UI调整：默认打开由 main() showMaximized() 最大化全屏
    setMinimumSize(900, 700);

    // ★ UI 查询数据库（单例，与服务共享同一实例）
    {
        m_pQueryDb = &SortingDatabase::instance();
        QString dbPath = QCoreApplication::applicationDirPath() + "/" + SORTING_DB_FILE;
        m_pQueryDb->open(dbPath);  // 若已打开则跳过
    }

    setupUI();
    ConfigManager::instance()->load();
    applyConfig();
    setupConnections();
    setupCore();   // ★ 2026-09-06 解耦：常驻实例 + 一次性配置/信号 + 设备(PLC/RFID)自动连接

#if AUTO_START_RECEIVE_ON_BOOT
    appendLog("程序已启动：PLC/RFID 设备自动连接中；即将自动开始接收任务"
              "（开机自动执行一次，之后的启停仍由按钮控制）");
#else
    appendLog("程序已启动：PLC/RFID 设备自动连接中；任务接收未开始——"
              "点击「开始接收任务」后 WMS 才可下发任务");
#endif

    // ★ 创建日志刷新定时器（100ms，防高频场景下 QTextEdit 卡死）
    m_logFlushTimer = new QTimer(this);
    m_logFlushTimer->setInterval(LOG_FLUSH_INTERVAL_MS);
    connect(m_logFlushTimer, &QTimer::timeout, this, &MainWindow::flushLogBuffer);
    m_logFlushTimer->start();

    // ★ 2026-09-08 开机自动接收（AUTO_START_RECEIVE_ON_BOOT=1）：
    //   程序启动后自动执行一次「开始接收任务」= 等价人工点击一次（省去开机首点）；
    //   仅本会话执行一次，之后的「结束任务/再次开始」仍完全由按钮照常控制。
    //   用 singleShot(0) 投递到事件循环：窗口已显示、setupCore() 设备层已启动后才执行。
#if AUTO_START_RECEIVE_ON_BOOT
    QTimer::singleShot(0, this, [this]() {
        if (m_bRunning || m_stopPhase != StopNone) return;   // 防御：已在接收/停止流程中不重复触发
        appendLog("[启动] 开机自动接收：自动执行一次「开始接收任务」（后续启停仍由按钮控制）");
        onStartStop();
    });
#endif

    // ★ 2026-09-06 解耦设计：设备(PLC/RFID)已在 setupCore() 自动连接并常驻；
    //   ★ 2026-09-08：接收层在开机时自动开始一次（AUTO_START_RECEIVE_ON_BOOT=1），
    //   之后「结束任务 / 再次开始接收」照常由按钮控制，设备层不受影响。
}

MainWindow::~MainWindow()
{
    // ★ 2026-09-07 防"关闭必崩"：先断开 PLC/S7/RFID 等设备 → 本窗口的所有信号连接，
    //   避免析构过程中子设备仍在 emit（queued/direct）打到本窗口已半析构的槽/lambda
    //   （历史 dmp 栈：~HttpServer→PlcManager::stop 期间 → updatePlcPanel→QLabel::setText→abort）
    if (m_pPlcMgr) m_pPlcMgr->disconnect(this);
    if (m_pServer) m_pServer->disconnect(this);
    // ★ 2026-09-08 UI调整：断开 RFID 推送实时表信号（防析构期间排队事件回调半析构对象）
    if (m_pServer && m_pServer->rfidPush()) m_pServer->rfidPush()->disconnect(this);

    // ★ 2026-09-06 解耦改造：HttpServer/HttpClient 常驻（parent=this），随本窗口析构自动销毁。
    //   此处先停接收层（HTTP 停止 + 兜底落库，幂等），设备层(PLC/S7/RFID/解析线程)由
    //   ~HttpServer 按 8 步析构日志收尾，保证退出顺序稳定、可排查。
    if (m_pServer) { m_pServer->stopReceive(); }
    if (m_timerRefresh) m_timerRefresh->stop();
    if (m_logFlushTimer)
    {
        m_logFlushTimer->stop();
        // 析构前最后一次刷新，确保日志不丢失
        flushLogBuffer();
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_bRunning || m_stopPhase == StopEnding)
    {
        auto ret = QMessageBox::question(this, "确认退出",
            m_bRunning
                ? QString("正在接收任务，确定退出吗？\n（建议先点击\"结束任务\"完成完结回传；退出后设备连接将断开）")
                : QString("正在停止接收（完结回传未完成），确定退出吗？\n（H8 未确认消息将在下次开始接收时自动补传）"),
            QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes)
        {
            event->ignore();
            return;
        }
        // ★ 2026-09-02：退出前执行停止收尾（幂等），避免服务/线程随窗口析构残留
        // ★ 2026-09-06 解耦：此处仅停接收层；设备层由 ~HttpServer 收尾
        doActualStop();
    }
    // ★ 2026-09-02 修复：未运行时关闭窗口不再调用 onStartStop()
    //   （原实现会误走"启动"分支，意外创建 HttpServer/HttpClient 后再随窗口销毁，存在崩溃风险）
    event->accept();
}

// ============================================================================
// ★ 2026-09-08 UI调整：默认最大化后，把各行水平分隔条按"两大部分各占一半"
//   布置一次（第一行 = 左半(任务接收控制|设备状态) | 波次信息，各占整行一半）。
//   在 changeEvent 收到 WindowStateChange 且窗口已最大化时触发（此时几何已确定），
//   只执行一次，之后仍可手动拖动分隔条。
// ============================================================================
void MainWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::WindowStateChange && isMaximized() && !m_defaultColSplitApplied)
    {
        m_defaultColSplitApplied = true;
        QTimer::singleShot(0, this, [this]() { applyDefaultColumnWidths(); });
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::applyDefaultColumnWidths()
{
    // 按分隔条当前实际宽度等分（扣除手柄宽度；各面板最小宽度不足一半时由 Qt 就近分配）
    auto halfSplit = [](QSplitter* sp) {
        if (!sp || sp->count() < 2) return;
        const int handles = sp->handleWidth() * (sp->count() - 1);
        const int avail = sp->width() - handles;
        if (avail <= 0) return;
        QList<int> sizes;
        const int each = avail / sp->count();
        for (int i = 0; i < sp->count() - 1; ++i)
            sizes << each;
        sizes << avail - each * (sp->count() - 1);   // 余数给最后一项
        sp->setSizes(sizes);
    };

    halfSplit(m_rowTopSplit);    // 左半 | 波次信息 —— 各占整行一半
    halfSplit(m_rowLogSplit);    // 运行日志 | 容器绑定状态
    halfSplit(m_rowQuerySplit);  // 分拣记录查询 | 波次数据记录
    halfSplit(m_rowLiveSplit);   // RFID推送数据 | PLC落格反馈数据
    // m_rowTopInner 不强制等分：任务接收控制保持内容宽度，设备状态吃满左半余量
}

void MainWindow::setupUI()
{
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout* mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(2);

    // ═══════════════════════════════════════════
    // 第一行：任务接收控制区（★ 2026-09-06 按钮只控制 WMS 任务接收；设备连接常驻）
    // ═══════════════════════════════════════════
    QGroupBox* grpServer = new QGroupBox(QCoreApplication::translate("MainWindow", "任务接收控制"));
    QVBoxLayout* serverLayout = new QVBoxLayout(grpServer);
    serverLayout->setAlignment(Qt::AlignCenter);

    // ★ 2026-09-06 解耦：按钮=控制「任务接收」开关；设备(PLC/RFID)连接随程序启动常驻
    m_btnStartStop = new QPushButton(QCoreApplication::translate("MainWindow", "开始接收任务"));
    m_btnStartStop->setMinimumWidth(120);
    m_btnStartStop->setMinimumHeight(36);
    m_btnStartStop->setStyleSheet(
        "QPushButton { background-color: #4CAF50; color: white; font-size: 14px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #45a049; }");

    m_lblServerStatus = new QLabel(QCoreApplication::translate("MainWindow", "● 未接收任务"));
    m_lblServerStatus->setStyleSheet("font-size: 14px; color: #f44336;");
    m_lblServerStatus->setAlignment(Qt::AlignCenter);

    // ★ 端口标签直接从配置读取（避免硬编码，确保重启后显示正确）
    ConfigManager* cfgMgr = ConfigManager::instance();
    cfgMgr->load();
    m_lblPort = new QLabel(QString("端口: %1").arg(cfgMgr->config().wmsListenPort));
    m_lblPort->setAlignment(Qt::AlignCenter);

    // ★ 期望绑定数量输入（默认1，每批次可配置不同数量）
    //   ★ 2026-09-08 UI需求1：该组控件**界面上不再显示**（值仍由 XML expectedBindCount 生效）；
    //   控件保留创建与配置同步逻辑，便于后续需要时一行恢复显示
    QHBoxLayout* bindCountRow = new QHBoxLayout();
    m_lblBindCountHint = new QLabel(QCoreApplication::translate("MainWindow", "期望绑定数量:"));
    m_lblBindCountHint->setStyleSheet("font-size: 13px;");
    m_spinBindCount = new QSpinBox();
    m_spinBindCount->setMinimum(1);
    m_spinBindCount->setMaximum(999);
    m_spinBindCount->setValue(cfgMgr->config().expectedBindCount);
    m_spinBindCount->setToolTip(QCoreApplication::translate("MainWindow", "波次下发时校验绑定数量，默认1。每批次可修改"));
    m_spinBindCount->setStyleSheet("QSpinBox { font-size: 13px; padding: 2px; }");
    m_spinBindCount->setFixedWidth(80);
    bindCountRow->addStretch();
    bindCountRow->addWidget(m_lblBindCountHint);
    bindCountRow->addWidget(m_spinBindCount);
    bindCountRow->addStretch();

    serverLayout->addWidget(m_btnStartStop, 0, Qt::AlignHCenter);

    // ★ 2026-09-07 布局：「未接收任务」状态 + 端口（水平同一行）
    QHBoxLayout* statusPortRow = new QHBoxLayout();
    statusPortRow->addStretch();
    statusPortRow->addWidget(m_lblServerStatus);
    statusPortRow->addWidget(m_lblPort);
    statusPortRow->addStretch();
    serverLayout->addLayout(statusPortRow);

    // ★ 2026-09-08 UI需求1：期望绑定数量行不加入布局（隐藏显示，配置项照常生效）
    // serverLayout->addLayout(bindCountRow);

    // ★ 2026-09-07 设置按钮：弹出 XML 配置编辑，保存即热生效（无需重启程序）
    //   ★ 2026-09-08 UI更新：布局改为与「查看接收波次队列」同一水平行（见下方 viewQueueRow）
    QPushButton* btnSettings = new QPushButton(QCoreApplication::translate("MainWindow", "设置配置"));
    btnSettings->setMinimumHeight(30);
    btnSettings->setStyleSheet(
        "QPushButton { background-color: #607D8B; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #546E7A; }");
    connect(btnSettings, &QPushButton::clicked, this, &MainWindow::openConfigEditor);

    // ★ 2026-09-06 回传保障按钮（主工作流不受影响；点击=按当前目标波次主动补发对应报文）
    //   目标波次：优先「波次数据记录」列表选中行；未选中时用当前内存波次
    // ★ 2026-09-07 重传满箱切换(H7) 旁新增「格口号输入框」：填了格口号 → 手动满箱切换
    //   （读取该格口当前分拣记录+容器号，按 H7 立即上传）；不填 → 原有重传行为
    // ★ 2026-09-07 布局纠正：H8 在上与「设置配置」同一行；H7 在下（带格口号输入框）
    m_btnResendH7 = new QPushButton(QCoreApplication::translate("MainWindow", "重传满箱切换(H7)"));
    m_btnResendH8 = new QPushButton(QCoreApplication::translate("MainWindow", "重传任务完结(H8)"));
    // ★ 2026-09-08 UI需求2/3：格口号输入框 → 失败格口下拉（可编辑：可手输任意格口）；
    //   H8 旁边新增失败波次下拉；两者数据来自 outbox 表中 failed/cancelled 报文
    m_cmbFailedH7 = new QComboBox();
    m_cmbFailedH7->setEditable(true);
    m_cmbFailedH7->setInsertPolicy(QComboBox::NoInsert);
    m_cmbFailedH7->setMinimumWidth(240);
    m_cmbFailedH7->setStyleSheet("QComboBox { font-size: 12px; padding: 2px 4px; }");
    if (m_cmbFailedH7->lineEdit())
        m_cmbFailedH7->lineEdit()->setPlaceholderText(
            QCoreApplication::translate("MainWindow", "选择失败格口，或手输格口号"));
    m_cmbFailedH7->setToolTip(QCoreApplication::translate("MainWindow",
        "下拉=全部历史失败/已取消重试的满箱报文（格口·波次·失败条数）：选中后点按钮按该格口精确重传；\n"
        "也可直接手输格口号：对当前波次该格口的分拣记录生成新的 H7 满箱回传并上传"));
    m_cmbFailedH8 = new QComboBox();
    m_cmbFailedH8->setInsertPolicy(QComboBox::NoInsert);
    m_cmbFailedH8->setMinimumWidth(200);
    m_cmbFailedH8->setStyleSheet("QComboBox { font-size: 12px; padding: 2px 4px; }");
    m_cmbFailedH8->setToolTip(QCoreApplication::translate("MainWindow",
        "下拉=全部历史失败/已取消重试的完结回传报文（波次·失败条数）：\n"
        "选中后点按钮只重传该波次的失败 H8，不影响主流程"));
    m_btnResendH7->setMinimumHeight(30);
    m_btnResendH8->setMinimumHeight(30);
    m_btnResendH7->setStyleSheet(
        "QPushButton { background-color: #FF9800; color: white; font-size: 12px; font-weight: bold; "
        "border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #F57C00; }");
    m_btnResendH8->setStyleSheet(
        "QPushButton { background-color: #8E24AA; color: white; font-size: 12px; font-weight: bold; "
        "border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #7B1FA2; }");
    // ── 第1行：查看接收波次队列（★ 2026-09-08 UI需求7：位于「重传任务完结」上方）──
    m_btnViewWaveQueue = new QPushButton(QCoreApplication::translate("MainWindow", "查看接收波次队列"));
    m_btnViewWaveQueue->setMinimumHeight(30);
    m_btnViewWaveQueue->setStyleSheet(
        "QPushButton { background-color: #0097A7; color: white; font-size: 12px; font-weight: bold; "
        "border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #00838F; }");
    m_btnViewWaveQueue->setToolTip(QCoreApplication::translate("MainWindow",
        "查看剩余待执行波次队列（含「接收新任务」选项：不处理排队波次，直接开始新任务）"));
    // ── 第1行：查看接收波次队列 + 设置配置（★ 2026-09-08 UI更新：两者同一水平行）──
    QHBoxLayout* viewQueueRow = new QHBoxLayout();
    viewQueueRow->addStretch();
    viewQueueRow->addWidget(m_btnViewWaveQueue);
    viewQueueRow->addWidget(btnSettings);   // ★ 2026-09-08「设置配置」移到与「查看接收波次队列」同一行
    viewQueueRow->addStretch();
    serverLayout->addLayout(viewQueueRow);

    // ── 第2行：重传任务完结(H8) + 失败波次下拉（同一水平行）──
    QHBoxLayout* resendRowH8 = new QHBoxLayout();
    resendRowH8->addStretch();
    resendRowH8->addWidget(m_btnResendH8);
    resendRowH8->addWidget(m_cmbFailedH8);   // ★ 2026-09-08 失败波次下拉
    resendRowH8->addStretch();
    serverLayout->addLayout(resendRowH8);

    // ── 第3行：重传满箱切换(H7) + 失败格口下拉（H7 在 H8 下方）──
    QHBoxLayout* resendRowH7 = new QHBoxLayout();
    resendRowH7->addStretch();
    resendRowH7->addWidget(m_btnResendH7);
    resendRowH7->addWidget(m_cmbFailedH7);   // ★ 2026-09-08 失败格口下拉（可编辑）
    resendRowH7->addStretch();
    serverLayout->addLayout(resendRowH7);

    // ★ 重传目标说明（选中行优先，否则当前内存波次——在 onResendSelectedH7/H8 中解析；
    //   下拉选中失败记录时按所选格口/波次精确重传；H7 下拉手输文本时执行手动满箱切换）
    connect(m_btnResendH7, &QPushButton::clicked, this, &MainWindow::onResendSelectedH7);
    connect(m_btnResendH8, &QPushButton::clicked, this, &MainWindow::onResendSelectedH8);
    connect(m_btnViewWaveQueue, &QPushButton::clicked, this, &MainWindow::onViewWaveQueue);

    // ═══════════════════════════════════════════
    // 第一行（中栏）：设备状态面板（PLC TCP / S7 / RFID，★ 2026-09-06 设备随程序启动常驻）
    // ═══════════════════════════════════════════
    QGroupBox* grpPlc = new QGroupBox(QCoreApplication::translate("MainWindow", "设备状态 (PLC/RFID)"));
    QVBoxLayout* plcOuterLayout = new QVBoxLayout(grpPlc);
    plcOuterLayout->setSpacing(4);

    // ── TCP 连接状态 ──
    QHBoxLayout* tcpRow1 = new QHBoxLayout();
    m_lblTcpStatus = new QLabel(QCoreApplication::translate("MainWindow", "TCP: 未连接"));
    m_lblTcpStatus->setStyleSheet("font-size: 13px; color: #888; font-weight: bold;");
    m_lblTcpIp = new QLabel("");
    m_lblTcpIp->setStyleSheet("font-size: 13px; color: #2196F3;");
    m_lblTcpUptime = new QLabel("");
    m_lblTcpUptime->setStyleSheet("font-size: 13px; color: #2196F3;");
    tcpRow1->addWidget(m_lblTcpStatus);
    tcpRow1->addWidget(m_lblTcpIp);
    tcpRow1->addWidget(m_lblTcpUptime);
    tcpRow1->addStretch();

    // ── TCP 收发统计 ──
    QHBoxLayout* tcpRow2 = new QHBoxLayout();
    m_lblTcpSend = new QLabel("TCP发送: 0");
    m_lblTcpSend->setStyleSheet("font-size: 13px; color: #2196F3;");
    m_lblTcpSendErr = new QLabel("TCP失败: 0");
    m_lblTcpSendErr->setStyleSheet("font-size: 13px; color: #f44336;");
    m_lblTcpRecv = new QLabel("TCP接收: 0");
    m_lblTcpRecv->setStyleSheet("font-size: 13px; color: #2196F3;");
    m_lblTcpConnCount = new QLabel("客户端: 0");
    m_lblTcpConnCount->setStyleSheet("font-size: 13px; color: #2196F3;");
    tcpRow2->addWidget(m_lblTcpSend);
    tcpRow2->addWidget(m_lblTcpSendErr);
    tcpRow2->addWidget(new QLabel("|"));
    tcpRow2->addWidget(m_lblTcpRecv);
    tcpRow2->addWidget(new QLabel("|"));
    tcpRow2->addWidget(m_lblTcpConnCount);
    tcpRow2->addStretch();

    // ── 分隔线 ──
    QFrame* lineS7 = new QFrame();
    lineS7->setFrameShape(QFrame::HLine);
    lineS7->setFrameShadow(QFrame::Sunken);

    // ── S7 连接状态 ──
    QHBoxLayout* s7Row1 = new QHBoxLayout();
    m_lblS7Status = new QLabel(QCoreApplication::translate("MainWindow", "S7: 未连接"));
    m_lblS7Status->setStyleSheet("font-size: 13px; color: #888; font-weight: bold;");
    m_lblS7Ip = new QLabel("");
    m_lblS7Ip->setStyleSheet("font-size: 13px; color: #2196F3;");
    s7Row1->addWidget(m_lblS7Status);
    s7Row1->addWidget(m_lblS7Ip);
    s7Row1->addStretch();

    // ── S7 收发统计 ──
    QHBoxLayout* s7Row2 = new QHBoxLayout();
    m_lblS7Send = new QLabel("S7发送: 0");
    m_lblS7Send->setStyleSheet("font-size: 13px; color: #2196F3;");
    m_lblS7SendErr = new QLabel("S7失败: 0");
    m_lblS7SendErr->setStyleSheet("font-size: 13px; color: #f44336;");
    m_lblS7LockGrids = new QLabel("锁格: 0");
    m_lblS7LockGrids->setStyleSheet("font-size: 13px; color: #FF5722; font-weight: bold;");
    s7Row2->addWidget(m_lblS7Send);
    s7Row2->addWidget(m_lblS7SendErr);
    s7Row2->addWidget(new QLabel("|"));
    s7Row2->addWidget(m_lblS7LockGrids);
    s7Row2->addStretch();

    // ── 分隔线 ──
    QFrame* lineRfid = new QFrame();
    lineRfid->setFrameShape(QFrame::HLine);
    lineRfid->setFrameShadow(QFrame::Sunken);

    // ── RFID 连接状态（★ 2026-09-06 WCS作客户端主动连接 RFID 服务端，随程序启动常驻）──
    QHBoxLayout* rfidRow = new QHBoxLayout();
    m_lblRfidStatus = new QLabel(QCoreApplication::translate("MainWindow", "RFID: 连接中..."));
    m_lblRfidStatus->setStyleSheet("font-size: 13px; color: #888; font-weight: bold;");
    ConfigManager* rfidCfg = ConfigManager::instance();
    m_lblRfidIp = new QLabel(QString("%1:%2")
        .arg(rfidCfg->config().rfidPushServerIp)
        .arg(rfidCfg->config().rfidPushServerPort));
    m_lblRfidIp->setStyleSheet("font-size: 13px; color: #888;");
    rfidRow->addWidget(m_lblRfidStatus);
    rfidRow->addWidget(m_lblRfidIp);
    rfidRow->addStretch();

    // ── 最近数据 ──
    QHBoxLayout* lastDataRow1 = new QHBoxLayout();
    m_lblLastSendCode = new QLabel(QCoreApplication::translate("MainWindow", "最近发送: --"));
    m_lblLastSendCode->setStyleSheet("font-size: 12px; color: #555;");
    m_lblLastSendGrid = new QLabel("");
    m_lblLastSendGrid->setStyleSheet("font-size: 12px; color: #2196F3;");
    m_lblLastSendTime = new QLabel("");
    m_lblLastSendTime->setStyleSheet("font-size: 12px; color: #888;");
    lastDataRow1->addWidget(m_lblLastSendCode);
    lastDataRow1->addWidget(m_lblLastSendGrid);
    lastDataRow1->addWidget(m_lblLastSendTime);
    lastDataRow1->addStretch();

    QHBoxLayout* lastDataRow2 = new QHBoxLayout();
    m_lblLastRecvCode = new QLabel(QCoreApplication::translate("MainWindow", "最近接收: --"));
    m_lblLastRecvCode->setStyleSheet("font-size: 12px; color: #555;");
    m_lblLastRecvGrid = new QLabel("");
    m_lblLastRecvGrid->setStyleSheet("font-size: 12px; color: #2196F3;");
    m_lblLastRecvTime = new QLabel("");
    m_lblLastRecvTime->setStyleSheet("font-size: 12px; color: #888;");
    lastDataRow2->addWidget(m_lblLastRecvCode);
    lastDataRow2->addWidget(m_lblLastRecvGrid);
    lastDataRow2->addWidget(m_lblLastRecvTime);
    lastDataRow2->addStretch();

    // 分隔线
    QFrame* line1 = new QFrame();
    line1->setFrameShape(QFrame::HLine);
    line1->setFrameShadow(QFrame::Sunken);

    plcOuterLayout->addLayout(tcpRow1);
    plcOuterLayout->addLayout(tcpRow2);
    plcOuterLayout->addWidget(lineS7);
    plcOuterLayout->addLayout(s7Row1);
    plcOuterLayout->addLayout(s7Row2);
    plcOuterLayout->addWidget(lineRfid);
    plcOuterLayout->addLayout(rfidRow);
    plcOuterLayout->addWidget(line1);
    plcOuterLayout->addLayout(lastDataRow1);
    plcOuterLayout->addLayout(lastDataRow2);

    // ═══════════════════════════════════════════
    // 第一行（与任务接收控制/设备状态同水平）：波次信息面板
    // ★ 2026-09-08 UI调整：字段改为两列成对排布，降低首行占用高度
    // ═══════════════════════════════════════════
    QGroupBox* grpWave = new QGroupBox("波次信息");
    QGridLayout* waveLayout = new QGridLayout(grpWave);

    auto makeLabel = [](const QString& title) {
        QLabel* label = new QLabel(title);
        label->setStyleSheet("font-size: 13px; min-width: 100px;");
        return label;
    };
    auto makeValue = []() {
        QLabel* label = new QLabel("--");
        label->setStyleSheet("font-size: 13px; font-weight: bold; color: #2196F3;");
        return label;
    };

    m_lblWaveCode    = makeValue();
    m_lblWaveStatus  = makeValue();
    m_lblSkuCount    = makeValue();
    m_lblSorted      = makeValue();
    m_lblException   = makeValue();
    m_lblSumLocation = makeValue();
    m_lblLastWave    = makeValue();
    m_lblEfficiency  = makeValue();   // ★ 2026-09-07 分拣效率
    m_lblPeakEff     = makeValue();   // ★ 2026-09-07 峰值效率（当日最大）

    // ★ 2026-09-08 UI调整：两列成对排布（每行左/右各一组 标签+值）
    int row = 0;
    auto addFieldPair = [&](const QString& t1, QLabel* v1, const QString& t2, QLabel* v2) {
        waveLayout->addWidget(makeLabel(t1), row, 0);
        waveLayout->addWidget(v1,            row, 1);
        if (v2) {
            waveLayout->addWidget(makeLabel(t2), row, 2);
            waveLayout->addWidget(v2,            row, 3);
        }
        ++row;
    };
    addFieldPair("波次号:",   m_lblWaveCode,    "状态:",     m_lblWaveStatus);
    addFieldPair("SKU数:",    m_lblSkuCount,    "已分拣:",   m_lblSorted);
    addFieldPair("异常:",     m_lblException,   "分拣件数:", m_lblSumLocation);
    addFieldPair("效率:",     m_lblEfficiency,  "峰值效率:", m_lblPeakEff);      // ★ 件/时 / 当日峰值 件/时
    addFieldPair("上波次:",   m_lblLastWave,    QString(),   nullptr);           // 末行右侧留空
    waveLayout->setColumnStretch(1, 1);   // 左值列占满剩余宽度
    waveLayout->setColumnStretch(3, 1);   // 右值列占满剩余宽度

    // ★ 开始分拣按钮（始终可见，到达可开始分拣状态时激活，否则灰色禁用）
    m_btnStartSorting = new QPushButton(QCoreApplication::translate("MainWindow", "开始分拣"));
    m_btnStartSorting->setStyleSheet(
        "QPushButton { font-size: 14px; font-weight: bold; padding: 6px 20px; "
        "background-color: #FF9800; color: white; border: none; border-radius: 4px; } "
        "QPushButton:hover { background-color: #F57C00; } "
        "QPushButton:disabled { background-color: #BDBDBD; }");
    m_btnStartSorting->setEnabled(false);  // 初始灰色禁用，到达 BOUND 状态时激活
    waveLayout->addWidget(m_btnStartSorting, row++, 0, 1, 4);   // ★ 2026-09-08 按钮跨整行（4列）

    // ═══════════════════════════════════════════
    // 第二行（右栏）：容器绑定状态面板（66格口，4列×17行网格）
    // ═══════════════════════════════════════════
    QGroupBox* grpBinding = new QGroupBox("容器绑定状态");
    QVBoxLayout* bindOuterLayout = new QVBoxLayout(grpBinding);

    QHBoxLayout* bindBtnRow = new QHBoxLayout();
    QPushButton* btnRefreshBind = new QPushButton("刷新绑定状态");
    btnRefreshBind->setMinimumHeight(30);
    btnRefreshBind->setStyleSheet(
        "QPushButton { background-color: #2196F3; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #1976D2; }");
    // ★ 2026-09-07 清空格口容器绑定（人工重置：内存清空 + DB 归档留史 + 恢复禁用格口）
    QPushButton* btnClearBinds = new QPushButton("清空格口绑定");
    btnClearBinds->setMinimumHeight(30);
    btnClearBinds->setStyleSheet(
        "QPushButton { background-color: #E53935; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #C62828; }");
    btnClearBinds->setToolTip(QString::fromUtf8("清空全部格口当前容器绑定（恢复初始状态）；历史记录归档保留在数据库，可追溯/可沿用"));
    // ★ 2026-09-11 图例补充橙色态（已解锁·待重绑）
    QLabel* bindHint = new QLabel(QString::fromUtf8("绿色=已绑定  灰色=未绑定  黄色=满箱锁格  橙色=已解锁·待重绑"));
    bindHint->setStyleSheet("font-size: 12px; color: #888;");
    bindBtnRow->addWidget(btnRefreshBind);
    bindBtnRow->addWidget(btnClearBinds);
    bindBtnRow->addWidget(bindHint);
    bindBtnRow->addStretch();

    // 已绑定/已锁格/未绑定计数
    m_lblBoundCount = new QLabel();
    m_lblLockedCount = new QLabel();   // ★ 2026-09-11：已锁格数量（黄色）
    m_lblUnboundCount = new QLabel();
    m_lblBoundCount->setStyleSheet("font-size: 13px; font-weight: bold; color: #4CAF50; padding: 0 8px;");
    // ★ 黄色 = 与格口"锁格"黄标（#FFC107 底、白字）同色系的计数标：底同色 + 深琥珀字（小字号可读）
    m_lblLockedCount->setStyleSheet(
        "font-size: 13px; font-weight: bold; color: #5D4000; background-color: #FFC107;"
        " border-radius: 7px; padding: 1px 8px;");
    m_lblUnboundCount->setStyleSheet("font-size: 13px; font-weight: bold; color: #E53935; padding: 0 8px;");
    bindBtnRow->addWidget(m_lblBoundCount);
    bindBtnRow->addWidget(m_lblLockedCount);   // 紧跟「已绑定」显示
    bindBtnRow->addWidget(m_lblUnboundCount);

    connect(btnRefreshBind, &QPushButton::clicked, this, &MainWindow::onRefreshBindings);
    connect(btnClearBinds,  &QPushButton::clicked, this, &MainWindow::onClearAllGridBinds);

    // 可滚动区域——容纳所有格口绑定指示器
    // ★ 2026-09-08 UI调整：不再固定180px，随第二行行高伸缩（保留下限）
    QScrollArea* scrollBinding = new QScrollArea();
    scrollBinding->setWidgetResizable(true);
    scrollBinding->setMinimumHeight(150);
    scrollBinding->setStyleSheet("QScrollArea { border: 1px solid #ddd; }");

    m_bindingWidget = new QWidget();
    m_bindingGrid = new QGridLayout(m_bindingWidget);
    m_bindingGrid->setSpacing(2);
    m_bindingGrid->setContentsMargins(4, 4, 4, 4);

    // 创建66个格口绑定标签（6列×11行）
    for (int i = 0; i < BINDING_SLOT_COUNT; ++i)
    {
        int gridNum = i + 1;
        int col = i % m_bindingCols;
        int row = i / m_bindingCols;

        // 每个格口一个 Frame 包裹
        QFrame* frame = new QFrame();
        frame->setFrameShape(QFrame::Box);
        frame->setStyleSheet("QFrame { background: #f5f5f5; border: 1px solid #ddd; border-radius: 2px; }");
        frame->setMinimumHeight(36);

        QHBoxLayout* fLayout = new QHBoxLayout(frame);
        fLayout->setContentsMargins(2, 1, 2, 1);
        fLayout->setSpacing(1);

        // ★ 格口标签：优先使用 XML 自定义名，否则零填充序号
        QString paddedNum = QString("%1").arg(gridNum, GRID_KEY_PADDING, 10, QChar('0'));
        QString displayName = ConfigManager::instance()->config().gridNames.value(QString::number(gridNum), paddedNum);
        QLabel* lblGrid = new QLabel(displayName);
        lblGrid->setFixedWidth(80);
        lblGrid->setAlignment(Qt::AlignCenter);
        lblGrid->setStyleSheet("font-size: 11px; font-weight: bold; color: #333; border: none; background: transparent;");

        // 绑定状态指示器标签
        QLabel* lblStatus = new QLabel("--");
        lblStatus->setFixedWidth(12);
        lblStatus->setFixedHeight(12);
        lblStatus->setAlignment(Qt::AlignCenter);
        lblStatus->setStyleSheet(
            "font-size: 10px; color: white; border-radius: 6px; background-color: #bbb;");
        lblStatus->setToolTip(QString("格口%1: 未绑定").arg(
            QString("%1").arg(gridNum, GRID_KEY_PADDING, 10, QChar('0'))));

        QLabel* lblBox = new QLabel("--");
        lblBox->setStyleSheet("font-size: 11px; color: #888; border: none; background: transparent;");
        lblBox->setMinimumWidth(90);

        fLayout->addWidget(lblGrid);
        fLayout->addWidget(lblStatus);
        fLayout->addWidget(lblBox);

        m_bindingGrid->addWidget(frame, row, col);

        // ★ 存储标签指针（后续刷新直接索引，避免 findChildren 递归查找）
        m_bindingLabels[i]    = lblStatus;
        m_bindingBoxLabels[i] = lblBox;
    }

    scrollBinding->setWidget(m_bindingWidget);
    bindOuterLayout->addLayout(bindBtnRow);
    bindOuterLayout->addWidget(scrollBinding);

    // ═══════════════════════════════════════════
    // 第三行（右栏）：★ 2026-09-06 波次数据记录面板：全部已传输波次
    //   刷新（手动） | 切换选中波次（恢复进度继续/终态载入查看） | 新任务（保留当前波次进度，清空待接收）
    //   H7/H8 重传按钮位于「任务接收控制」区（主工作流保障，不随本面板操作）
    // ═══════════════════════════════════════════
    QGroupBox* grpUnfinished = new QGroupBox(QCoreApplication::translate("MainWindow", "波次数据记录（全部已传输波次）"));
    QVBoxLayout* unfinishedLayout = new QVBoxLayout(grpUnfinished);

    QHBoxLayout* unfinishedBtnRow = new QHBoxLayout();
    m_btnRefreshWaves = new QPushButton(QCoreApplication::translate("MainWindow", "刷新"));
    m_btnResumeWave   = new QPushButton(QCoreApplication::translate("MainWindow", "切换选中波次"));
    m_btnNewTask      = new QPushButton(QCoreApplication::translate("MainWindow", "新任务"));
    m_btnRefreshWaves->setMinimumHeight(30);
    m_btnResumeWave->setMinimumHeight(30);
    m_btnNewTask->setMinimumHeight(30);
    m_btnRefreshWaves->setStyleSheet(
        "QPushButton { background-color: #2196F3; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #1976D2; }");
    m_btnResumeWave->setStyleSheet(
        "QPushButton { background-color: #4CAF50; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #388E3C; }");
    m_btnNewTask->setStyleSheet(
        "QPushButton { background-color: #FF5722; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #E64A19; }");
    unfinishedBtnRow->addWidget(m_btnRefreshWaves);
    unfinishedBtnRow->addWidget(m_btnResumeWave);
    unfinishedBtnRow->addWidget(m_btnNewTask);
    unfinishedBtnRow->addStretch();

    m_tblWaveRecords = new QTableWidget();
    m_tblWaveRecords->setColumnCount(8);
    m_tblWaveRecords->setHorizontalHeaderLabels(
        QStringList() << "波次号" << "状态" << "件数" << "已分拣" << "异常" << "H7满箱" << "H8完结" << "更新时间");
    m_tblWaveRecords->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tblWaveRecords->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tblWaveRecords->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tblWaveRecords->horizontalHeader()->setStretchLastSection(true);
    m_tblWaveRecords->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tblWaveRecords->setMinimumHeight(200);
    // ★ 双击行 = 切换选中波次
    connect(m_tblWaveRecords, &QTableWidget::cellDoubleClicked, this,
        [this](int, int) { onResumeSelectedWave(); });

    unfinishedLayout->addLayout(unfinishedBtnRow);
    unfinishedLayout->addWidget(m_tblWaveRecords);

    connect(m_btnRefreshWaves, &QPushButton::clicked, this, &MainWindow::onRefreshWaveRecords);
    connect(m_btnResumeWave,   &QPushButton::clicked, this, &MainWindow::onResumeSelectedWave);
    connect(m_btnNewTask,      &QPushButton::clicked, this, &MainWindow::onStartNewWaveTask);

    // ═══════════════════════════════════════════
    // 第三行（左栏）：★ 分拣记录查询面板
    // ═══════════════════════════════════════════
    QGroupBox* grpQuery = new QGroupBox(QCoreApplication::translate("MainWindow", "分拣记录查询"));
    QVBoxLayout* queryLayout = new QVBoxLayout(grpQuery);

    // ── 查询条件行 ──
    QHBoxLayout* queryCondRow = new QHBoxLayout();

    // ★ 查询模式下拉框
    queryCondRow->addWidget(new QLabel(QCoreApplication::translate("MainWindow", "查询模式:")));
    m_cmbQueryMode = new QComboBox();
    m_cmbQueryMode->addItem(QCoreApplication::translate("MainWindow", "按EPC查询"));
    m_cmbQueryMode->addItem(QCoreApplication::translate("MainWindow", "按SKU查询格口"));
    m_cmbQueryMode->addItem(QCoreApplication::translate("MainWindow", "按格口查询"));   // ★ 2026-09-09 需求2
    m_cmbQueryMode->setMinimumWidth(140);
    queryCondRow->addWidget(m_cmbQueryMode);

    // ★ EPC编码输入（默认显示）
    m_editQueryBarcode = new QLineEdit();
    m_editQueryBarcode->setPlaceholderText(QCoreApplication::translate("MainWindow", "输入EPC编码查询（留空查全部）"));
    m_editQueryBarcode->setMinimumWidth(180);
    queryCondRow->addWidget(m_editQueryBarcode);

    // ★ SKU编码输入（默认隐藏，按SKU查询时显示）
    m_editQuerySku = new QLineEdit();
    m_editQuerySku->setPlaceholderText(QCoreApplication::translate("MainWindow", "输入SKU编码（显示分配格口 + 每个EPC的实际落格号）"));
    m_editQuerySku->setMinimumWidth(180);
    m_editQuerySku->setVisible(false);
    queryCondRow->addWidget(m_editQuerySku);

    queryCondRow->addWidget(new QLabel(QCoreApplication::translate("MainWindow", "日期:")));
    m_editQueryDateFrom = new QDateEdit(QDate::currentDate().addDays(-7));
    m_editQueryDateFrom->setCalendarPopup(true);
    m_editQueryDateFrom->setDisplayFormat("yyyy-MM-dd");
    queryCondRow->addWidget(m_editQueryDateFrom);

    queryCondRow->addWidget(new QLabel("~"));
    m_editQueryDateTo = new QDateEdit(QDate::currentDate());
    m_editQueryDateTo->setCalendarPopup(true);
    m_editQueryDateTo->setDisplayFormat("yyyy-MM-dd");
    queryCondRow->addWidget(m_editQueryDateTo);

    m_btnQueryRecords = new QPushButton(QCoreApplication::translate("MainWindow", "查询"));
    m_btnQueryRecords->setMinimumHeight(32);
    m_btnQueryRecords->setStyleSheet(
        "QPushButton { background-color: #2196F3; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #1976D2; }");
    queryCondRow->addWidget(m_btnQueryRecords);

    m_btnQueryClear = new QPushButton(QCoreApplication::translate("MainWindow", "清空"));
    m_btnQueryClear->setMinimumHeight(32);
    queryCondRow->addWidget(m_btnQueryClear);

    // ★ 2026-09-07 效率统计按钮：弹出 RFID 推送效率统计图（独立弹窗，不影响主界面）
    m_btnEffChart = new QPushButton(QCoreApplication::translate("MainWindow", "效率统计"));
    m_btnEffChart->setMinimumHeight(32);
    m_btnEffChart->setStyleSheet(
        "QPushButton { background-color: #26A69A; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 14px; }"
        "QPushButton:hover { background-color: #00897B; }");
    m_btnEffChart->setToolTip(QCoreApplication::translate("MainWindow",
        "弹出 RFID 推送效率统计图：最近30分钟柱状（每分钟件数） + 今日0~23点峰值折线（件/时）"));
    queryCondRow->addWidget(m_btnEffChart);
    queryCondRow->addStretch();

    // ── 统计标签 ──
    QHBoxLayout* statsRow = new QHBoxLayout();
    m_lblRecordCount = new QLabel(QCoreApplication::translate("MainWindow", "共 0 条记录"));
    m_lblRecordCount->setStyleSheet("font-size: 12px; color: #555;");
    m_lblDbStats = new QLabel("");
    m_lblDbStats->setStyleSheet("font-size: 12px; color: #2196F3;");
    statsRow->addWidget(m_lblRecordCount);
    statsRow->addWidget(m_lblDbStats);
    statsRow->addStretch();

    // ── 结果表格 ──
    m_tblRecords = new QTableWidget();
    m_tblRecords->setColumnCount(10);
    m_tblRecords->setHorizontalHeaderLabels({
        QCoreApplication::translate("MainWindow", "序号"),
        QCoreApplication::translate("MainWindow", "波次号"),
        QCoreApplication::translate("MainWindow", "EPC编码"),
        QCoreApplication::translate("MainWindow", "SKU编码"),
        QCoreApplication::translate("MainWindow", "格口号"),
        QCoreApplication::translate("MainWindow", "小车号(首车/尾车)"),
        QCoreApplication::translate("MainWindow", "件数"),
        QCoreApplication::translate("MainWindow", "库位"),
        QCoreApplication::translate("MainWindow", "分拣时间"),
        QCoreApplication::translate("MainWindow", "状态")
    });
    m_tblRecords->setMinimumHeight(180);
    m_tblRecords->setMaximumHeight(300);
    m_tblRecords->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tblRecords->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tblRecords->setAlternatingRowColors(true);
    m_tblRecords->horizontalHeader()->setStretchLastSection(true);
    m_tblRecords->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tblRecords->verticalHeader()->setVisible(false);
    m_tblRecords->setStyleSheet(
        "QTableWidget { font-size: 12px; }"
        "QTableWidget::item { padding: 2px 4px; }"
        "QHeaderView::section { background-color: #e0e0e0; font-weight: bold; padding: 4px; }");

    queryLayout->addLayout(queryCondRow);
    queryLayout->addLayout(statsRow);
    queryLayout->addWidget(m_tblRecords);

    // 连接信号
    connect(m_btnQueryRecords, &QPushButton::clicked, this, &MainWindow::onQueryRecords);
    connect(m_btnQueryClear,   &QPushButton::clicked, this, [this]() {
        m_tblRecords->setRowCount(0);
        m_lblRecordCount->setText(QCoreApplication::translate("MainWindow", "共 0 条记录"));
        m_lblDbStats->setText("");
    });
    connect(m_btnEffChart, &QPushButton::clicked, this, &MainWindow::onOpenEffChart);
    // 回车触发查询
    connect(m_editQueryBarcode, &QLineEdit::returnPressed, this, &MainWindow::onQueryRecords);
    connect(m_editQuerySku,     &QLineEdit::returnPressed, this, &MainWindow::onQueryRecords);

    // ★ 查询模式切换：显示/隐藏对应输入框（0=EPC, 1=SKU, 2=按格口 复用EPC输入框）
    connect(m_cmbQueryMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        bool isSkuMode = (index == 1);
        m_editQueryBarcode->setVisible(!isSkuMode);
        m_editQuerySku->setVisible(isSkuMode);
        if (index == 2)
            m_editQueryBarcode->setPlaceholderText(QString::fromUtf8("输入格口号查询该格明细（7 / 007 / 22007 均可；留空查全格口汇总）"));
        else
            m_editQueryBarcode->setPlaceholderText(QString::fromUtf8("输入EPC编码查询（留空查全部）"));
    });

    // ═══════════════════════════════════════════
    // 第二行（左栏）：日志区
    // ═══════════════════════════════════════════
    QGroupBox* grpLog = new QGroupBox("运行日志");
    QVBoxLayout* logLayout = new QVBoxLayout(grpLog);

    m_txtLog = new QTextEdit();
    m_txtLog->setReadOnly(true);
    m_txtLog->document()->setMaximumBlockCount(LOG_MAX_BLOCK_COUNT);
    m_txtLog->setStyleSheet("font-family: Consolas, 'Microsoft YaHei'; font-size: 12px;");

    QPushButton* btnClearLog = new QPushButton("清空日志");

    logLayout->addWidget(m_txtLog);
    logLayout->addWidget(btnClearLog);
    connect(btnClearLog, &QPushButton::clicked, this, &MainWindow::onClearLog);

    // ═══════════════════════════════════════════
    // ★ 2026-09-08 UI调整 第四行：RFID推送数据 / PLC落格反馈数据 —— 实时滚动显示
    //   数据源：RFID = RfidPushClient::rfidPushReceived（QueuedConnection 回主线程）
    //           PLC  = PlcManager::plcFeedbackBatch（复用现有100ms批量信号）
    //   新行插第0行（最新在最上），超 LIVE_TABLE_MAX_ROWS 行自动裁掉最旧行
    // ═══════════════════════════════════════════
    auto styleLiveTable = [](QTableWidget* tbl, const QStringList& headers) {
        tbl->setColumnCount(headers.size());
        tbl->setHorizontalHeaderLabels(headers);
        tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
        tbl->setSelectionMode(QAbstractItemView::SingleSelection);
        tbl->setAlternatingRowColors(true);
        tbl->setShowGrid(true);
        tbl->verticalHeader()->setVisible(false);
        tbl->horizontalHeader()->setStretchLastSection(false);
        tbl->horizontalHeader()->setHighlightSections(false);
        tbl->setMinimumHeight(140);
        tbl->setStyleSheet(
            "QTableWidget { font-size: 12px; }"
            "QTableWidget::item { padding: 1px 4px; }"
            "QHeaderView::section { background-color: #e0e0e0; font-weight: bold; padding: 3px; }");
    };

    // ── RFID 推送数据实时表：序号 | 时间 | EPC编码 | 小车号 ──
    QGroupBox* grpRfidLive = new QGroupBox("RFID推送数据（实时）");
    QVBoxLayout* rfidLiveLayout = new QVBoxLayout(grpRfidLive);
    rfidLiveLayout->setContentsMargins(6, 4, 6, 4);
    m_tblRfidPush = new QTableWidget();
    styleLiveTable(m_tblRfidPush, QStringList() << "序号" << "时间" << "EPC编码" << "小车号");
    m_tblRfidPush->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_tblRfidPush->setColumnWidth(0, 64);
    m_tblRfidPush->setColumnWidth(1, 100);
    m_tblRfidPush->setColumnWidth(2, 160);
    m_tblRfidPush->setColumnWidth(3, 100);
    m_tblRfidPush->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);  // EPC列吃余量
    rfidLiveLayout->addWidget(m_tblRfidPush);

    // ── PLC 落格反馈数据实时表：序号 | 时间 | EPC编码 | 格口号 | 小车号(首车/尾车) | 状态码 ──
    QGroupBox* grpPlcLive = new QGroupBox("PLC落格反馈数据（实时）");
    QVBoxLayout* plcLiveLayout = new QVBoxLayout(grpPlcLive);
    plcLiveLayout->setContentsMargins(6, 4, 6, 4);
    m_tblPlcFeedback = new QTableWidget();
    styleLiveTable(m_tblPlcFeedback, QStringList()
        << "序号" << "时间" << "EPC编码" << "格口号" << "小车号(首车/尾车)" << "状态码");
    m_tblPlcFeedback->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_tblPlcFeedback->setColumnWidth(0, 64);
    m_tblPlcFeedback->setColumnWidth(1, 100);
    m_tblPlcFeedback->setColumnWidth(2, 150);
    m_tblPlcFeedback->setColumnWidth(3, 90);
    m_tblPlcFeedback->setColumnWidth(4, 170);
    m_tblPlcFeedback->setColumnWidth(5, 110);
    m_tblPlcFeedback->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);  // EPC列吃余量
    plcLiveLayout->addWidget(m_tblPlcFeedback);

    // ═══════════════════════════════════════════
    // 组装布局（★ 2026-09-07 QSplitter：行内水平与整体垂直分隔条均可手动拖动）
    // ★ 2026-09-08 UI看板调整：
    //   第一行：任务接收控制 ｜ 设备状态(PLC/RFID) ｜ 波次信息
    //   第二行：运行日志      ｜ 容器绑定状态
    //   第三行：分拣记录查询  ｜ 波次数据记录（全部已传输波次）
    //   第四行：RFID推送数据（实时） ｜ PLC落格反馈数据（实时）
    // ═══════════════════════════════════════════
    QSplitter* vsplit = new QSplitter(Qt::Vertical, central);
    vsplit->setChildrenCollapsible(false);
    vsplit->setHandleWidth(5);

    // 第一行：任务接收控制 + 设备状态(PLC/RFID) + 波次信息
    // ★ 2026-09-08 UI调整：波次信息默认占整行一半 → 左侧两栏包成内层分隔条，
    //   外层 = 左半 | 波次信息（默认 1:1）；内层 = 任务接收控制 | 设备状态（控制区按内容宽）
    QSplitter* rowTopInner = new QSplitter(Qt::Horizontal);
    rowTopInner->setChildrenCollapsible(false);
    rowTopInner->setHandleWidth(5);
    rowTopInner->addWidget(grpServer);
    rowTopInner->addWidget(grpPlc);
    rowTopInner->setStretchFactor(0, 0);   // 任务接收控制按内容宽度
    rowTopInner->setStretchFactor(1, 1);   // 设备状态吃满左半余量

    QSplitter* rowTopSplit = new QSplitter(Qt::Horizontal);
    rowTopSplit->setChildrenCollapsible(false);
    rowTopSplit->setHandleWidth(5);
    rowTopSplit->addWidget(rowTopInner);
    rowTopSplit->addWidget(grpWave);
    rowTopSplit->setStretchFactor(0, 1);
    rowTopSplit->setStretchFactor(1, 1);   // 波次信息：整行右侧一半（默认）
    vsplit->addWidget(rowTopSplit);
    m_rowTopInner = rowTopInner;
    m_rowTopSplit = rowTopSplit;

    // 第二行：运行日志 + 容器绑定状态 —— 水平可拖动（★ 2026-09-08 默认各占一半）
    QSplitter* rowLogSplit = new QSplitter(Qt::Horizontal);
    rowLogSplit->setChildrenCollapsible(false);
    rowLogSplit->setHandleWidth(5);
    rowLogSplit->addWidget(grpLog);
    rowLogSplit->addWidget(grpBinding);   // ★ 2026-09-08 容器绑定状态移到第二行右侧
    rowLogSplit->setStretchFactor(0, 1);
    rowLogSplit->setStretchFactor(1, 1);
    vsplit->addWidget(rowLogSplit);
    m_rowLogSplit = rowLogSplit;

    // 第三行：分拣记录查询 + 波次数据记录 —— 水平可拖动（★ 2026-09-08 默认各占一半）
    QSplitter* rowQuerySplit = new QSplitter(Qt::Horizontal);
    rowQuerySplit->setChildrenCollapsible(false);
    rowQuerySplit->setHandleWidth(5);
    rowQuerySplit->addWidget(grpQuery);
    rowQuerySplit->addWidget(grpUnfinished);   // ★ 2026-09-08 波次数据记录移到第三行右侧
    rowQuerySplit->setStretchFactor(0, 1);
    rowQuerySplit->setStretchFactor(1, 1);
    vsplit->addWidget(rowQuerySplit);
    m_rowQuerySplit = rowQuerySplit;

    // 第四行：RFID推送数据 + PLC落格反馈数据 —— 实时滚动表（★ 2026-09-08 默认各占一半）
    QSplitter* rowLiveSplit = new QSplitter(Qt::Horizontal);
    rowLiveSplit->setChildrenCollapsible(false);
    rowLiveSplit->setHandleWidth(5);
    rowLiveSplit->addWidget(grpRfidLive);
    rowLiveSplit->addWidget(grpPlcLive);
    rowLiveSplit->setStretchFactor(0, 1);
    rowLiveSplit->setStretchFactor(1, 1);
    vsplit->addWidget(rowLiveSplit);
    m_rowLiveSplit = rowLiveSplit;

    // 垂直分配：首行按内容（stretch 0），其余行 1:1:2 优先给实时表；初始比例见 setSizes
    vsplit->setStretchFactor(vsplit->indexOf(rowLogSplit),   1);
    vsplit->setStretchFactor(vsplit->indexOf(rowQuerySplit), 1);
    vsplit->setStretchFactor(vsplit->indexOf(rowLiveSplit),  2);
    vsplit->setSizes({ 260, 250, 220, 320 });   // 初始高度（窗口尺寸变化时按比例缩放）

    mainLayout->addWidget(vsplit, 1);
}

void MainWindow::setupConnections()
{
    connect(m_btnStartStop, &QPushButton::clicked, this, &MainWindow::onStartStop);

    // ★ 期望绑定数量变更 → 保存到配置，并实时更新到 HttpServer
    connect(m_spinBindCount, QOverload<int>::of(&QSpinBox::valueChanged), this,
        [this](int value) {
            AppConfig& cfg = ConfigManager::instance()->config();
            cfg.expectedBindCount = value;
            ConfigManager::instance()->save();  // 延迟保存到 XML
            if (m_pServer)
                m_pServer->setExpectedBindCount(value);
            appendLog(QString("[配置] 期望绑定数量已更新: %1").arg(value));
        });

    // 定时刷新（每秒）
    m_timerRefresh = new QTimer(this);
    connect(m_timerRefresh, &QTimer::timeout, this, &MainWindow::onRefreshTimer);
    m_timerRefresh->start(UI_REFRESH_INTERVAL_MS);

    // ★ 开始分拣按钮
    connect(m_btnStartSorting, &QPushButton::clicked, this, &MainWindow::onStartSortingClicked);
}

void MainWindow::applyConfig()
{
    AppConfig& cfg = ConfigManager::instance()->config();
    m_runtimeWmsPort = cfg.wmsListenPort;   // ★ 启动时的监听端口（配置热更新提示用）
    m_lblPort->setText(QString("端口: %1").arg(cfg.wmsListenPort));
    if (m_spinBindCount)
        m_spinBindCount->setValue(cfg.expectedBindCount);
}

// ============================================================================
// ★ 2026-09-07 应用"可热生效"配置项（保存配置后调用，无需重启）：
//   回传 URL/AppKey/method、环境开关、HTTP超时、RFID查询地址/鉴权、RFID心跳开关/间隔、
//   波次超时/重试、期望绑定数。
//   端口/PLC地址/线程池/格口显示名等需重启生效（界面已提示）。
// ============================================================================
void MainWindow::applyLiveConfig()
{
    AppConfig& cfg = ConfigManager::instance()->config();

    if (m_pClient)
    {
        m_pClient->setUrl(cfg.activeFeedbackUrl());
        m_pClient->setEndUrl(cfg.activeEndFeedbackUrl());
        m_pClient->setAppkey(cfg.activeAppkey());
        m_pClient->setFeedbackMethod(cfg.feedbackMethod);
        m_pClient->setEndFeedbackMethod(cfg.feedbackEndMethod);
        m_pClient->setTimeout(cfg.httpTimeoutMs);
        m_pClient->setRfidQueryUrl(cfg.rfidQueryUrl);
        m_pClient->setRfidAppkey(cfg.rfidAppkey);
    }
    if (m_pServer)
    {
        if (m_pServer->waveManager())
        {
            m_pServer->waveManager()->setWaveTimeoutMin(cfg.waveTimeoutMin);
            m_pServer->waveManager()->setMaxRetry(cfg.maxRetryCount);
        }
        m_pServer->setExpectedBindCount(cfg.expectedBindCount);
        if (m_pServer->rfidPush())
        {
            m_pServer->rfidPush()->setHeartbeatEnabled(cfg.rfidHeartbeatEnable != 0);
            m_pServer->rfidPush()->setHeartbeatIntervalMs(cfg.rfidHeartbeatIntervalMs);
        }
    }
    if (m_spinBindCount)
        m_spinBindCount->setValue(cfg.expectedBindCount);

    // 端口类展示：实际监听未变时明确标注，避免误导
    if (m_runtimeWmsPort > 0 && cfg.wmsListenPort != m_runtimeWmsPort)
    {
        m_lblPort->setText(QString("端口: %1 (监听仍 %2，重启生效)")
            .arg(cfg.wmsListenPort).arg(m_runtimeWmsPort));
    }
    else
    {
        m_lblPort->setText(QString("端口: %1 %2")
            .arg(cfg.wmsListenPort)
            .arg(cfg.useTestEnv ? QString::fromUtf8("(测试)") : QString::fromUtf8("(正式)")));
    }

    WCS_LOG_INFO("配置热生效应用完成（回传URL/AppKey/method、环境、HTTP超时、RFID查询/心跳、波次参数、期望绑定数）");
    appendLog("[配置] 热生效应用完成：回传 URL/AppKey/method、环境开关、HTTP超时、RFID查询/心跳、"
              "波次超时/重试、期望绑定数 已按新配置更新（端口/IP/线程池类需重启生效）");
}

// ============================================================================
// ★ 2026-09-07 设置按钮：弹出 XML 配置编辑对话框
//   保存：XML 校验 → 备份旧文件(.bak_时间) → 写盘 → 重新加载 → 热生效可热更项
// ============================================================================
void MainWindow::openConfigEditor()
{
    QString cfgPath = QCoreApplication::applicationDirPath() + "/config/http_server.xml";

    QFile f(cfgPath);
    if (!f.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(this, QString::fromUtf8("设置配置"),
            QString::fromUtf8("无法读取配置文件：\n%1").arg(cfgPath));
        return;
    }
    QString xmlText = QString::fromUtf8(f.readAll());
    f.close();

    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8("配置编辑（保存后热生效，无需重启）"));
    dlg.resize(960, 700);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    QLabel* tip = new QLabel(QString::fromUtf8(
        "配置文件：%1\n\n"
        "保存后立即生效：回传URL/AppKey/method、环境开关(useTestEnv)、HTTP超时、RFID查询地址/鉴权、\n"
        "RFID心跳开关/间隔、波次超时、重试次数、期望绑定数量、重扫重投(rescanResend*)。\n"
        "需重启生效：WMS监听端口、PLC地址/端口、RFID服务端IP/端口、线程池大小、格口显示名。\n"
        "编辑窗口打开期间请勿同时执行会写配置的操作（如绑定变更），以免被覆盖。")
        .arg(cfgPath));
    tip->setWordWrap(true);
    tip->setStyleSheet("font-size: 12px; color: #555;");

    QPlainTextEdit* ed = new QPlainTextEdit();
    ed->setPlainText(xmlText);
    ed->setLineWrapMode(QPlainTextEdit::NoWrap);
    ed->setStyleSheet("QPlainTextEdit { font-family: Consolas,'Microsoft YaHei'; font-size: 13px; }");

    QDialogButtonBox* box = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    box->button(QDialogButtonBox::Save)->setText(QString::fromUtf8("保存并生效"));
    box->button(QDialogButtonBox::Cancel)->setText(QString::fromUtf8("取消"));

    lay->addWidget(tip);
    lay->addWidget(ed, 1);
    lay->addWidget(box);

    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(box, &QDialogButtonBox::accepted, &dlg, [&dlg, &ed, &cfgPath, this]() {
        QString newText = ed->toPlainText();

        // ── 1. XML 语法校验 ──
        {
            QXmlStreamReader xr(newText);
            while (!xr.atEnd())
            {
                xr.readNext();
                if (xr.hasError())
                {
                    QMessageBox::warning(&dlg, QString::fromUtf8("保存失败"),
                        QString::fromUtf8("XML 格式错误：%1（第 %2 行）\n请修正后再保存。")
                            .arg(xr.errorString()).arg(xr.lineNumber()));
                    return;
                }
            }
        }

        // ── 2. 备份旧配置 ──
        {
            QString bak = cfgPath + ".bak_" +
                QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
            QFile::copy(cfgPath, bak);
        }

        // ── 3. 写盘 ──
        QFile out(cfgPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            QMessageBox::warning(&dlg, QString::fromUtf8("保存失败"),
                QString::fromUtf8("无法写入配置文件：\n%1").arg(cfgPath));
            return;
        }
        out.write(newText.toUtf8());
        out.close();

        // ── 4. 重新加载 + 热生效 ──
        bool loadOk = ConfigManager::instance()->load();
        if (!loadOk)
        {
            QMessageBox::warning(&dlg, QString::fromUtf8("配置加载失败"),
                QString::fromUtf8("文件已保存，但重新解析失败（%1）。\n请检查内容或恢复备份文件。").arg(cfgPath));
            return;
        }
        applyLiveConfig();
        appendLog(QString("[配置] 已保存并重新加载：%1（旧文件已备份）").arg(cfgPath));

        QMessageBox::information(&dlg, QString::fromUtf8("已保存并生效"),
            QString::fromUtf8("配置已保存并热生效。\n\n"
                              "立即生效：回传URL/AppKey/method、环境开关、HTTP超时、RFID查询/心跳、波次参数等。\n"
                              "如需改动 监听端口/PLC地址/RFID地址/线程池 等项，请重启软件生效。"));
        dlg.accept();
    });

    dlg.exec();
}

// ============================================================================
// ★ 2026-09-06 设备连接与任务接收解耦改造：
//   setupCore() —— 程序启动时执行一次（构造函数末尾调用）：
//     ① 创建常驻 HttpServer/HttpClient（parent=this，随程序退出才析构，不再逐次启停）；
//     ② 一次性注入配置 + 连接全部信号（实例常驻，信号只连一次）；
//     ③ 自动启动设备层（PLC TCP 监听 / S7 锁格 / RFID 客户端 —— 常驻，与按钮无关）。
//   此后按钮仅控制「接收层」：开始接收任务=startReceive(port)、结束任务收尾=stopReceive()。
// ============================================================================
void MainWindow::setupCore()
{
    m_stopPhase = StopNone;

    // ★ 常驻实例：HttpServer 构造即打开数据库、建线程池/解析线程等（只此一次）
    m_pServer = new HttpServer(this);
    m_pClient = new HttpClient(this);
    m_pPlcMgr = m_pServer->plcManager();  // ★ 获取PLC管理器引用（生命周期由常驻实例管理）

    AppConfig& cfg = ConfigManager::instance()->config();

    // ★ 从配置文件恢复容器绑定（跨会话保留；结束任务后由 WMS 新一轮波次重新下发）
    m_pServer->loadContainerBindings(cfg.containerBindings);
    m_bindingDirty = true;  // ★ 初始加载后标记为脏，开始接收后首次刷新时更新面板

    // ★ 设置期望绑定数量（从配置文件加载，默认1）
    m_pServer->setExpectedBindCount(cfg.expectedBindCount);

    // ★ 回传客户端配置（H7/H8 回传 + RFID SKU-EPC 绑定查询）
    m_pClient->setUrl(cfg.activeFeedbackUrl());
    m_pClient->setEndUrl(cfg.activeEndFeedbackUrl());  // ★ 完结回传专用 URL
    m_pClient->setAppkey(cfg.activeAppkey());
    m_pClient->setFeedbackMethod(cfg.feedbackMethod);        // ★ 满箱/锁格/波次完成回传 method
    m_pClient->setEndFeedbackMethod(cfg.feedbackEndMethod);  // ★ 完结回传(H8) method
    m_pClient->setTimeout(cfg.httpTimeoutMs);
    m_pClient->setRfidQueryUrl(cfg.rfidQueryUrl);  // ★ RFID SKU-EPC 绑定查询 URL
    m_pClient->setRfidAppkey(cfg.rfidAppkey);      // ★ RFID 查询鉴权 AppKey
    m_pServer->setHttpClient(m_pClient);           // ★ 设置 HttpClient 供 RFID 查询使用

    // ★ 从配置文件加载 API 路由路径
    m_pServer->setApiInsertWaveInfo(cfg.apiInsertWaveInfo);
    m_pServer->setApiBindingLatticePort(cfg.apiBindingLatticePort);
    m_pServer->setApiInsertWaveIn(cfg.apiInsertWaveIn);

    m_pServer->waveManager()->setWaveTimeoutMin(cfg.waveTimeoutMin);
    m_pServer->waveManager()->setMaxRetry(cfg.maxRetryCount);

    // ══════════════════════════════════════════════════════════
    // ★ 一次性信号连接（实例常驻，以下 connect 均只执行一次）
    // ══════════════════════════════════════════════════════════

    // ★ 波次完成回传 → WMS（HttpServer 异步入池构建 JSON，HttpClient 发送）
    connect(m_pServer, &HttpServer::waveCompleteReportReady, this,
        [this](const QJsonObject& reportJson) {
            if (!m_pClient) return;
            QString orderCode = reportJson["head"].toObject()["orderCode"].toString();
            appendLog(QString("波次完成回传 orderCode=%1").arg(orderCode));
            m_pClient->sendGenericFeedback(reportJson, orderCode);
        });

    // ★ 回传结果处理：成功→已完成，失败→异常（避免状态卡在"回传中"）
    // S5 更新：区分完结回传（H8）和满箱回传（H7）
    connect(m_pClient, &HttpClient::reportResult, this,
        [this](const QString& orderCode, bool success, const QString& body) {
            Q_UNUSED(body);

            // ★ S5: 满箱回传（H7，context 以 "fullbox_" 开头）
            if (orderCode.startsWith("fullbox_"))
            {
                QString msgId = orderCode.mid(8); // 去掉 "fullbox_" 前缀
                if (m_pServer)
                {
                    m_pServer->onFullboxReplyFinished(msgId, success, body);
                }
                appendLog(QString("[满箱回传] 回传结果 msgId=%1 success=%2")
                    .arg(msgId).arg(success));
                return;
            }

            // ★锁格回传（context 以 "lockGrid_" 开头）
            if (orderCode.startsWith("lockGrid_"))
            {
                appendLog(QString("[锁格] 回传结果 grid=%1 success=%2")
                    .arg(orderCode.mid(9)).arg(success));
                return;
            }

            // ★完结回传 Outbox（H8，context 以 "end_" 开头）
            if (orderCode.startsWith("end_"))
            {
                QString msgId = orderCode.mid(4); // 去掉 "end_" 前缀
                if (m_pServer)
                {
                    m_pServer->onEndReplyFinished(msgId, success, body);
                }
                appendLog(QString("[完结回传] 回传结果 success=%1")
                    .arg(success));
                return;
            }

            // ★ 未完成波次面板手动重传（轻量，只更新 outbox 状态，不动波次状态/绑定）
            if (orderCode.startsWith("resendFullbox_"))
            {
                QString msgId = orderCode.mid(QString("resendFullbox_").length());
                if (m_pServer) m_pServer->onOutboxResendReply(msgId, true, success);
                appendLog(QString("[未完成波次] H7重传结果 success=%1").arg(success));
                return;
            }
            if (orderCode.startsWith("resendEnd_"))
            {
                QString msgId = orderCode.mid(QString("resendEnd_").length());
                if (m_pServer) m_pServer->onOutboxResendReply(msgId, false, success);
                appendLog(QString("[未完成波次] H8重传结果 success=%1").arg(success));
                return;
            }

            // ★ 波次完成回传（H7 锁格回传，原有逻辑）— 仅记录结果，不改变波次状态
            //   状态迁移仅由完结回传（H8，以 "end_" 为前缀）处理
            WaveManager* wm = m_pServer ? m_pServer->waveManager() : nullptr;
            if (!wm) return;
            if (success)
            {
                appendLog(QString("波次完成回传成功 orderCode=%1").arg(orderCode));
                // ★ 锁格回传（H7）成功不改变波次状态，状态由完结回传（H8）管理
            }
            else
            {
                appendLog(QString("波次完成回传失败 orderCode=%1（仍可手动重试）").arg(orderCode), true);
                // ★ 锁格回传（H7）失败不改变波次状态，状态由完结回传（H8）管理
            }
        });

    // ★ 锁格回传 → WMS（HttpServer 构建 JSON，HttpClient 发送）
    connect(m_pServer, &HttpServer::gridLockReportReady, this,
        [this](const QJsonObject& reportJson) {
            if (!m_pClient) return;
            QString grid = reportJson["head"].toObject()["detailList"].toArray().first()
                .toObject()["targetLocation"].toString();
            appendLog(QString("[锁格] 发送回传 grid=%1").arg(grid));
            m_pClient->sendGenericFeedback(reportJson, "lockGrid_" + grid);
        });

    // ★ S5 满箱回传 → WMS（H7 满箱同步，T-S5-04）
    // fullboxReportReady 携带 msgId，HttpClient 返回后路由到 onFullboxReplyFinished
    connect(m_pServer, &HttpServer::fullboxReportReady, this,
        [this](const QJsonObject& payload, const QString& msgId) {
            if (!m_pClient) return;
            QString orderCode = payload["head"].toObject()["orderCode"].toString();
            appendLog(QString("[满箱回传] 发送回传 order=%1").arg(orderCode));
            m_pClient->sendGenericFeedback(payload, "fullbox_" + msgId);
        });

    // ★ S6 完结回传 → WMS（H8 波次完结通知，T-S6-03）
    // endReportReady 携带 msgId，HttpClient 返回后路由到 onEndReplyFinished
    connect(m_pServer, &HttpServer::endReportReady, this,
        [this](const QJsonObject& payload, const QString& msgId) {
            if (!m_pClient) return;
            QString orderCode = payload["head"].toObject()["orderCode"].toString();
            appendLog(QString("[完结回传] 发送回传 order=%1").arg(orderCode));
            m_pClient->sendEndFeedback(payload, "end_" + msgId);  // ★ H8 使用专用完结回传 URL
        });

    // ★ 未完成波次面板：手动重传 H7/H8（走 HttpClient，结果经 reportResult 的 resend* 前缀路由）
    connect(m_pServer, &HttpServer::outboxResendReady, this,
        [this](const QString& kind, const QJsonObject& payload, const QString& msgId) {
            if (!m_pClient) return;
            if (kind == "fullbox")
                m_pClient->sendGenericFeedback(payload, "resendFullbox_" + msgId);
            else if (kind == "end")
                m_pClient->sendEndFeedback(payload, "resendEnd_" + msgId);
        });

    // ★ 波次记录面板：重传结果回执 → 日志 + 刷新列表（事件反馈，非轮询）
    connect(m_pServer, &HttpServer::outboxResendResult, this,
        [this](const QString& orderCode, const QString& kind, const QString& msgId, bool success) {
            Q_UNUSED(msgId);
            appendLog(QString("[重传] %1 补发%2 order=%3")
                .arg(kind == "fullbox" ? "满箱切换(H7)" : "任务完结(H8)")
                .arg(success ? "成功" : "失败")
                .arg(orderCode));
            onRefreshWaveRecords();
        });

    // ★ 上一波次恢复完成 → 刷新波次面板
    connect(m_pServer, &HttpServer::waveResumed, this,
        [this](const QString& orderCode, int status) {
            Q_UNUSED(orderCode);
            updateWavePanel();
            appendLog(QString("[恢复] 波次面板已刷新 状态=%1")
                .arg(WaveSnapshot::statusToString(status)));
        });

    // ★ 2026-09-08 UI需求5/7：待执行波次队列变化 → 刷新波次数据记录列表（含「排队待执行」标注与按钮计数）
    connect(m_pServer, &HttpServer::pendingWavesChanged, this, [this]() {
        onRefreshWaveRecords();
    });

    // ★ 2026-09-08 UI需求2/3：失败重传记录变化（重试耗尽/手动重传成功/切出取消重试）→ 刷新两个下拉
    connect(m_pServer, &HttpServer::outboxFailedChanged, this, [this]() {
        refreshFailedCombos();
    });

    // ★ H8完结回传处理完毕 → 停止接收收尾
    //   ★ 2026-09-06 解耦：Outbox(H8)补传跨接收会话继续执行（设备/实例常驻），
    //     回传的最终结果可能在「下一轮开始接收」之后才到达——仅当本窗口正处于
    //     StopEnding（点击"结束任务"后等待中）时才停止接收，防止上一会话遗留回传
    //     结果误停新一轮接收；用户取消/超时路径由 doActualStop() 幂等兜底。
    connect(m_pServer, &HttpServer::endReportFinished, this, [this]() {
        if (m_stopPhase == StopEnding)
        {
            appendLog("[完结回传] 回传流程结束，正在停止任务接收...");
            doActualStop();
        }
        else
        {
            appendLog("[完结回传] 后台补传已结束（当前不在停止等待流程，保持接收状态不变）");
        }
    });

    // ★ 连接HttpServer日志信号到UI日志区
    connect(m_pServer, &HttpServer::logMessage, this, &MainWindow::appendLog);

    // ★ 容器绑定变更 → 即时刷新 UI + 持久化到 XML
    connect(m_pServer, &HttpServer::bindingUpdated, this, [this]() {
        m_bindingDirty = true;  // ★ 标记脏数据，下次定时刷新时更新
        updateBindingPanel();
        // 同步到配置并保存
        AppConfig& c = ConfigManager::instance()->config();
        c.containerBindings = m_pServer->getContainerBindings();
        ConfigManager::instance()->save();
    });

    // ★ 连接PLC状态信号到UI（全部使用 QueuedConnection，确保跨线程安全）
    if (m_pPlcMgr)
    {
        connect(m_pPlcMgr, &PlcManager::plcConnected, this, [this](const QString& ip, int port) {
            updatePlcPanel();
            appendLog(QString("[PLC] TCP连接 %1:%2").arg(ip).arg(port));
        }, Qt::QueuedConnection);
        connect(m_pPlcMgr, &PlcManager::plcDisconnected, this, [this](const QString& ip, int port) {
            updatePlcPanel();
            appendLog(QString("[PLC] TCP断开 %1:%2").arg(ip).arg(port), true);
        }, Qt::QueuedConnection);

        // ★ 业务信号：批量处理落格反馈计数
        //    不再逐条 connect plcFeedbackReceived，改用批量信号
        connect(m_pPlcMgr, &PlcManager::plcFeedbackBusinessBatch, this,
            [this](const QVector<PlcFeedbackEntry>& entries) {
                m_plcFeedbackCount.fetchAndAddRelaxed(entries.size());
            }, Qt::QueuedConnection);

        // ★ UI日志信号：批量处理，减少高频场景下的UI更新压力
        // ★ 2026-09-08 UI调整：同一次批量的反馈同时追加到「PLC落格反馈数据」实时表
        connect(m_pPlcMgr, &PlcManager::plcFeedbackBatch, this,
            [this](const QVector<PlcFeedbackEntry>& entries) {
                if (entries.isEmpty()) return;

                // ── 追加实时表（批量一次更新，减少重绘）──
                if (m_tblPlcFeedback)
                {
                    const bool heavy = entries.size() > 16;
                    if (heavy) m_tblPlcFeedback->setUpdatesEnabled(false);
                    for (const auto& e : entries)
                    {
                        const quint32 no = ++m_plcFeedbackSeq;
                        QString carTxt = e.lastCar.isEmpty()
                            ? e.car                                   // 3字段格式：只有单车号
                            : QString::fromUtf8("首:%1 尾:%2")        // 5字段格式：首车/尾车
                                .arg(e.firstCar, e.lastCar);
                        QString statusTxt;
                        bool bad = false;
                        // ★ 2026-09-09 需求3：状态码旁附加信息描述（0/1/2/3 为已知语义；7/8 待客户确认，先给占位描述）
                        switch (e.status)
                        {
                        case 0: statusTxt = QStringLiteral("0 —(3字段无状态)");      break;
                        case 1: statusTxt = QStringLiteral("1 成功");                 break;
                        case 2: statusTxt = QStringLiteral("2 无格口");   bad = true; break;
                        case 3: statusTxt = QStringLiteral("3 信息不全"); bad = true; break;
                        case 7: statusTxt = QStringLiteral("7 状态码7(待确认含义)");  break;
                        case 8: statusTxt = QStringLiteral("8 状态码8(待确认含义)");  break;
                        default: statusTxt = QString::fromUtf8("状态码%1(未知)").arg(e.status); break;
                        }
                        pushLiveRow(m_tblPlcFeedback,
                            QStringList()
                                << QString::number(no)
                                << QDateTime::fromMSecsSinceEpoch(e.timestampMs).toString("HH:mm:ss")
                                << e.code << e.grid << carTxt << statusTxt,
                            bad);
                    }
                    if (heavy) m_tblPlcFeedback->setUpdatesEnabled(true);
                    m_tblPlcFeedback->viewport()->update();
                }

                // ── 原有日志显示逻辑保持不变 ──
                if (entries.size() == 1)
                {
                    // 单条：直接显示
                    const auto& e = entries.first();
                    appendLog(QString("[PLC] 反馈落格 code=%1 → grid=%2 car=%3")
                        .arg(e.code).arg(e.grid).arg(e.car));
                }
                else
                {
                    // 多条：汇总显示前3条 + 共N条
                    QString summary;
                    int showCount = qMin(entries.size(), FEEDBACK_DISPLAY_MAX);
                    for (int i = 0; i < showCount; ++i)
                    {
                        const auto& e = entries[i];
                        if (i > 0) summary += "\n";
                        summary += QString("  code=%1 → grid=%2 car=%3")
                            .arg(e.code).arg(e.grid).arg(e.car);
                    }
                    if (entries.size() > FEEDBACK_DISPLAY_MAX)
                        summary += QString("\n  ... 共 %1 条").arg(entries.size());
                    appendLog(QString("[PLC] 批量反馈 (%1条):\n%2").arg(entries.size()).arg(summary));
                }
            }, Qt::QueuedConnection);

        connect(m_pPlcMgr, &PlcManager::plcBatchStart, this, [this]() {
            appendLog("[PLC] 批次开始");
        }, Qt::QueuedConnection);
        connect(m_pPlcMgr, &PlcManager::plcBatchStop, this, [this]() {
            appendLog("[PLC] 批次停止");
        }, Qt::QueuedConnection);
        // ★ 发送数据通知
        connect(m_pPlcMgr, &PlcManager::plcSendInfo, this,
            [this](const QString& code, const QString& grids, bool success) {
                QString status = success ? "✓" : "✗";
                appendLog(QString("[PLC] %1 code=%2 grids=%3").arg(status).arg(code).arg(grids));
            }, Qt::QueuedConnection);

        // ★ S7 信号（全部使用 QueuedConnection）
        connect(m_pPlcMgr, &PlcManager::s7Connected, this, [this](const QString& ip) {
            updatePlcPanel();
            appendLog(QString("[S7] 连接成功 %1").arg(ip));
        }, Qt::QueuedConnection);
        connect(m_pPlcMgr, &PlcManager::s7Disconnected, this, [this](const QString& ip) {
            updatePlcPanel();
            appendLog(QString("[S7] 断开 %1").arg(ip), true);
        }, Qt::QueuedConnection);
        connect(m_pPlcMgr, &PlcManager::s7Error, this, [this](const QString& errMsg) {
            appendLog(QString("[S7] 错误: %1").arg(errMsg), true);
        }, Qt::QueuedConnection);
        connect(m_pPlcMgr, &PlcManager::gridLocked, this, [this](const QString& grid) {
            updatePlcPanel();
            updateBindingPanel();  // ★ 锁格→黄色
            appendLog(QString("[S7] 锁格 grid=%1").arg(grid), true);
        }, Qt::QueuedConnection);
        connect(m_pPlcMgr, &PlcManager::gridUnlocked, this, [this](const QString& grid) {
            updatePlcPanel();
            updateBindingPanel();  // ★ 解锁→恢复绿/红
            appendLog(QString("[S7] 解锁 grid=%1").arg(grid));
        }, Qt::QueuedConnection);
    }

    // ★ 2026-09-08 UI调整：RFID推送数据实时表数据接入
    //   RfidPushClient::rfidPushReceived 在 HP-Socket 工作线程 emit（body.data[].epc/carNum），
    //   以 QueuedConnection 回主线程插表（先于 startDevices 连接，不丢后续推送帧）
    if (m_pServer && m_pServer->rfidPush())
    {
        connect(m_pServer->rfidPush(), &RfidPushClient::rfidPushReceived, this,
            [this](const QJsonObject& body) {
                if (!m_tblRfidPush) return;
                const QJsonArray arr = body.value("data").toArray();
                if (arr.isEmpty()) return;
                const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
                const bool heavy = arr.size() > 16;
                if (heavy) m_tblRfidPush->setUpdatesEnabled(false);
                for (const QJsonValue& v : arr)
                {
                    const QJsonObject o = v.toObject();
                    const QString epc = o.value("epc").toString();
                    if (epc.isEmpty()) continue;   // 与业务侧一致：NOREAD 等空 EPC 不展示
                    const quint32 no = ++m_rfidPushSeq;
                    pushLiveRow(m_tblRfidPush,
                        QStringList()
                            << QString::number(no) << ts
                            << epc << o.value("carNum").toString());
                }
                if (heavy) m_tblRfidPush->setUpdatesEnabled(true);
                m_tblRfidPush->viewport()->update();
            }, Qt::QueuedConnection);
    }

    // ══════════════════════════════════════════════════════════
    // ★ 设备层自动连接（程序启动即连，常驻；失败仅日志/UI提示，不阻断）
    // ══════════════════════════════════════════════════════════
    m_pServer->startDevices();

    // ★ 启动后清理过期数据库记录（一次性；日常清理由数据库内部定时器负责）
    if (m_pServer->sortingDb())
        m_pServer->sortingDb()->cleanupOldRecords(SORTING_DB_RETAIN_DAYS);

    // ★ 初始化完成（设备连接状态由每秒定时器刷新显示；波次记录列表初始填充一次，后续手动刷新）
    onRefreshWaveRecords();
    refreshFailedCombos();   // ★ 2026-09-08 初始化 H7 失败格口 / H8 失败波次两个下拉
    appendLog("[初始化] 设备层已启动（PLC/RFID 常驻）；任务接收未开始，请点击「开始接收任务」");
}

// ============================================================================
// 槽函数
// ============================================================================

void MainWindow::onStartStop()
{
    if (m_bRunning)//状态：开启 --> 关闭（点击"结束任务"）
    {
        // ★ 2026-09-02 修复"结束任务卡死/闪退"：
        //   ① 点击后触发 H8 完结回传，进入 StopEnding 等待阶段；
        //   ② 等待期间按钮保持可点，再次点击 = 取消等待、立即停止
        //      （H8 未确认的消息保留在 outbox_end，下次启动自动补传）；
        //   ③ 无论 H8 成功/失败耗尽/超时/用户取消，最终都走到幂等的 doActualStop()，
        //      服务停止后软件保持运行，不卡死、不闪退。
        if (m_stopPhase == StopEnding)
        {
            appendLog("[完结回传] 用户取消等待，立即停止接收（H8 未确认，下次开始接收时自动补传）", true);
            doActualStop();
            return;
        }
        m_stopPhase = StopEnding;

        // ★ 2026-09-02：sendEnd 返回 false = 无活跃波次/状态不允许完结（同步拒绝）
        //   → 无需等待回传，立即停止接收（不卡 30s 等待）
        if (m_pServer && !m_pServer->sendEnd())
        {
            appendLog("[完结回传] 无活跃波次或状态不允许完结，直接停止接收", true);
            m_stopPhase = StopNone;   // 允许 doActualStop 正常执行（非重复路径）
            doActualStop();
            return;
        }
        else if (!m_pServer)
        {
            appendLog("[完结回传] 任务接收未开始，无法触发完结回传", true);
            m_stopPhase = StopNone;
            doActualStop();
            return;
        }
        else
        {
            appendLog("[完结回传] 已触发完结回传（H8）");
        }

        // ★ 不立即停止接收，等待 H8 完结回传结果返回
        //   实际停止由 endReportFinished 信号 / 超时 / 用户取消 → doActualStop()
        // ★ 2026-09-06 解耦：设备(PLC/RFID)保持连接，仅停止 WMS 任务接收
        m_bRunning = false;
        m_btnStartStop->setEnabled(true);  // ★ 保持可点 = "立即停止"（不再禁用，避免无法取消）
        m_btnStartStop->setText(QCoreApplication::translate("MainWindow", "停止中…(点击立即停止)"));
        m_btnStartStop->setStyleSheet(
            "QPushButton { background-color: #FF9800; color: white; font-size: 14px; font-weight: bold; "
            "border-radius: 4px; padding: 6px 16px; }");
        m_lblServerStatus->setText(QCoreApplication::translate("MainWindow", "● 停止接收中"));
        m_lblServerStatus->setStyleSheet("font-size: 14px; color: #FF9800;");

        // ★ 安全网：END_WAIT_TIMEOUT_MS 超时后强制停止（doActualStop 幂等，可安全重复触发）
        if (!m_stopTimeoutTimer) {
            m_stopTimeoutTimer = new QTimer(this);
            m_stopTimeoutTimer->setSingleShot(true);
            connect(m_stopTimeoutTimer, &QTimer::timeout, this, [this]() {
                appendLog(QString("[完结回传] 等待超时(%1s)，强制停止服务（H8 未确认，下次启动自动补传）")
                              .arg(END_WAIT_TIMEOUT_MS / 1000), true);
                doActualStop();
            });
        }
        m_stopTimeoutTimer->start(END_WAIT_TIMEOUT_MS);  // 等待上限（不卡死）

        // ★ 2026-09-06 解耦：设备(PLC/S7/RFID)保持连接，此处不再复位设备面板
        //   （面板由每秒定时器实时刷新；状态变化才更新样式）
        // 波次面板数据保留至结束（H8 完结后由 doActualStop 复位）

        // ★ 容器绑定不再在「结束任务」时清除——绑定由数据库持久化，波次完结（H8成功）时由 HttpServer 统一清空；
        //   此处仅做绑定面板的视觉复位（H8 成功后 bindingUpdated 信号会再次刷新）
        // 重置绑定面板为全灰
        for (int i = 0; i < BINDING_SLOT_COUNT; ++i)
        {
            if (m_bindingLabels[i])
                m_bindingLabels[i]->setStyleSheet(
                    "font-size: 10px; color: white; border-radius: 6px; background-color: #bbb;");
            if (m_bindingBoxLabels[i])
                m_bindingBoxLabels[i]->setText("--");
        }
        if (m_lblBoundCount)  m_lblBoundCount->setText("已绑定: 0");
        if (m_lblLockedCount) m_lblLockedCount->setText(QString::fromUtf8("已锁格: 0"));
        if (m_lblUnboundCount) m_lblUnboundCount->setText("未绑定: 66");

        // ★ 结束任务时重置"开始分拣"按钮为初始灰色禁用状态
        m_btnStartSorting->setEnabled(false);
        m_bindingDirty = false;

        appendLog(QString("任务接收正在停止，等待完结回传结果（最长 %1s；可再次点击按钮立即停止）... 设备连接保持")
                      .arg(END_WAIT_TIMEOUT_MS / 1000));
    }
    else    //状态：未接收 --> 开始接收（★ 2026-09-06：仅启动「接收层」，设备层已在 setupCore 常驻）
    {
        // ★ 2026-09-02：新一轮接收会话，重置停止阶段（防上一轮 StopEnding 残留影响）
        m_stopPhase = StopNone;

        AppConfig& cfg = ConfigManager::instance()->config();
        int port = cfg.wmsListenPort;

        if (m_pServer->startReceive(port))
        {
            m_bRunning = true;
            m_btnStartStop->setText("结束任务");
            m_btnStartStop->setStyleSheet(
                "QPushButton { background-color: #f44336; color: white; font-size: 14px; font-weight: bold; "
                "border-radius: 4px; padding: 6px 16px; }"
                "QPushButton:hover { background-color: #d32f2f; }");
            m_lblServerStatus->setText("● 接收中");
            m_lblServerStatus->setStyleSheet("font-size: 14px; color: #4CAF50;");

            m_lblPort->setText(QString("端口: %1 %2")
                .arg(port)
                .arg(cfg.useTestEnv ? "(测试)" : "(正式)"));
            m_lblPort->setStyleSheet(cfg.useTestEnv
                ? "font-size: 14px; color: #FF9800; font-weight: bold;"
                : "font-size: 14px; color: #f44336; font-weight: bold;");

            appendLog(QString("任务接收已开始（HTTP 监听 %1，环境=%2）——等待 WMS 下发任务")
                .arg(port)
                .arg(cfg.useTestEnv ? "测试" : "正式"));

            // ★ 显示当前恢复的容器绑定（程序启动时从配置文件恢复；结束任务后由 WMS 重新下发）
            m_bindingDirty = true;
            updateBindingPanel();
            updatePlcPanel();
            updateRfidStatus();

            // ★ 配置摘要日志（每次开始接收时打印一次，便于排查）
            {
                //QString summary;
                //summary += "\n\n══════════════════ 配置摘要 ══════════════════\n\n";
                //summary += QString(" 监听端口:        %1 (WMS) / %2 (PLC)\n\n")
                //    .arg(port).arg(cfg.plcListenPort);
                //summary += QString(" 回传URL:         %1 (%2)\n\n")
                //    .arg(cfg.activeFeedbackUrl())
                //    .arg(cfg.useTestEnv ? "测试" : "正式");
                //summary += QString(" AppKey:          %1\n\n").arg(cfg.activeAppkey());
                //summary += QString(" 仓库:            %1\n\n").arg(cfg.warehouseCode);
                //summary += QString(" 货主:            %1\n\n").arg(cfg.goodsOwner);
                //summary += QString(" 波次超时:        %1分钟(%2), 期望绑定: %3\n\n")
                //    .arg(cfg.waveTimeoutMin)
                //    .arg(cfg.waveTimeoutMin == 0 ? "不超时" : QString::number(cfg.waveTimeoutMin) + "分钟")
                //    .arg(cfg.expectedBindCount);
                //summary += QString(" 重试:            %1次, 间隔: %2秒\n\n")
                //    .arg(OUTBOX_RETRY_MAX_DEFAULT).arg(OUTBOX_RETRY_INTERVAL_SEC);
                //summary += QString(" 日志:            保留%1天\n").arg(cfg.logRetainDays);
                //summary += QString(" 配置文件版本:    %1 (软件版本: %2)\n\n")
                //    .arg(cfg.configVersion).arg(CONFIG_VERSION);
                //if (cfg.configVersion != CONFIG_VERSION)
                //{
                //    summary += QString(" ⚠ 配置文件版本不匹配! 请检查配置\n\n");
                //}
                //summary += "══════════════════════════════════════════════\n\n";
                //appendLog(summary);

                appendLog("\n\n══════════════════ 配置摘要 ══════════════════\n\n");
                appendLog ( QString(" 监听端口:        %1 (WMS) / %2 (PLC)\n\n") .arg(port).arg(cfg.plcListenPort));
                appendLog ( QString(" 回传URL:         %1 (%2)\n\n").arg(cfg.activeFeedbackUrl()).arg(cfg.useTestEnv ? "测试" : "正式"));
                // ★ 2026-09-06：网关 appkey/method 参数展示（发送时拼到 URL）
                appendLog(QString(" 网关参数:        appkey=%1 method(满箱/其他)=%2 method(完结H8)=%3\n\n")
                    .arg(cfg.activeAppkey()).arg(cfg.feedbackMethod).arg(cfg.feedbackEndMethod));
                appendLog(QString(" AppKey:          %1\n\n").arg(cfg.activeAppkey()));
                appendLog(QString(" 仓库:            %1\n\n").arg(cfg.warehouseCode));
                appendLog(QString(" 货主:            %1\n\n").arg(cfg.goodsOwner));
                appendLog(QString(" 波次超时:        %1分钟(%2), 期望绑定: %3\n\n").arg(cfg.waveTimeoutMin).arg(cfg.waveTimeoutMin == 0 ? "不超时" : QString::number(cfg.waveTimeoutMin) + "分钟").arg(cfg.expectedBindCount));
                appendLog(QString(" 重试:            %1次, 间隔: %2秒\n\n").arg(OUTBOX_RETRY_MAX_DEFAULT).arg(OUTBOX_RETRY_INTERVAL_SEC));
                // ★ 2026-09-04 RFID 配置展示（方便现场排查 RFID 链路）
                appendLog(QString(" RFID查询接口:    %1\n\n").arg(cfg.rfidQueryUrl));
                appendLog(QString(" RFID推送服务端:  %1:%2 (WCS主动连接)\n\n").arg(cfg.rfidPushServerIp).arg(cfg.rfidPushServerPort));
                appendLog(QString(" 日志:            保留%1天\n").arg(cfg.logRetainDays));
                // ★ 2026-09-11 重扫重投口径（拿起已落格的件重新上料 → 仍按原格口下发）
                appendLog(QString(" 重扫重投:        %1 (冷却%2ms / 每波次上限%3次 / 在途超时%4ms)\n\n")
                    .arg(cfg.rescanResendEnabled ? QString::fromUtf8("开启") : QString::fromUtf8("关闭"))
                    .arg(cfg.rescanResendCooldownMs).arg(cfg.rescanResendMaxTimes).arg(cfg.plcInFlightTimeoutMs));
                appendLog(QString(" 配置文件版本:    %1 (软件版本: %2)\n\n").arg(cfg.configVersion).arg(CONFIG_VERSION));
                if (cfg.configVersion != CONFIG_VERSION)
                {
                    appendLog ( QString(" ⚠ 配置文件版本不匹配! 请检查配置\n\n"));
                }
                appendLog ( "\n══════════════════════════════════════════════\n\n");
               
                WCS_LOG_INFO("配置摘要: 端口=%d/%d URL=%s env=%s warehouse=%s goodsOwner=%s waveTimeout=%d bindCount=%d",
                    port, cfg.plcListenPort, cfg.activeFeedbackUrl().toLocal8Bit().data(),
                    cfg.useTestEnv ? "test" : "prod",
                    cfg.warehouseCode.toLocal8Bit().data(), cfg.goodsOwner.toLocal8Bit().data(),
                    cfg.waveTimeoutMin, cfg.expectedBindCount);
            }
        }
        else
        {
            appendLog("任务接收启动失败！", true);
            // ★ 2026-09-06：失败原因弹窗指引（端口占用最常见——旧实例未退出/双开）
            QString errText = QString("任务接收启动失败（HTTP 监听端口 %1 被占用）。\n\n"
                                      "最常见原因：端口被占用（上一个程序实例未退出）。\n\n"
                                      "处理方法：\n"
                                      "  ① 若弹过\"程序已在运行\"提示 → 使用旧实例即可\n"
                                      "  ② 任务管理器 → 结束所有 WCS_httpServer.exe → 重新打开\n"
                                      "  ③ 或重启电脑后打开")
                                  .arg(port);
            QMessageBox::warning(this, QString("接收启动失败"), errText);
            appendLog(QString("接收启动失败排查：请检查端口 %1 是否被占用（netstat -ano | findstr %1）").arg(port), true);
            // ★ 2026-09-06 解耦：HttpServer/HttpClient 为常驻实例，不随接收失败销毁
            //   （设备层 PLC/RFID 保持连接，可稍后再次点击「开始接收任务」）
            m_bRunning = false;
            m_lblServerStatus->setText(QCoreApplication::translate("MainWindow", "● 未接收任务"));
            m_lblServerStatus->setStyleSheet("font-size: 14px; color: #f44336;");
            m_btnStartStop->setEnabled(true);
            m_btnStartStop->setText(QCoreApplication::translate("MainWindow", "开始接收任务"));
            m_btnStartStop->setStyleSheet(
                "QPushButton { background-color: #4CAF50; color: white; font-size: 14px; font-weight: bold; "
                "border-radius: 4px; padding: 6px 16px; }"
                "QPushButton:hover { background-color: #45a049; }");
        }
    }
}

void MainWindow::onRefreshTimer()
{
    if (!m_pServer) return;

    // ★ 2026-09-06 解耦：设备状态（PLC TCP/S7/RFID）无论是否接收任务都每秒实时刷新
    updatePlcPanel();
    updateRfidStatus();

    if (m_bRunning && m_pServer->waveManager())
    {
        updateWavePanel();
        // ★ 仅绑定数据变更时才刷新绑定面板（避免每秒66次findChildren）
        if (m_bindingDirty)
        {
            updateBindingPanel();
            m_bindingDirty = false;
        }
        // ★ 更新数据库统计（每10秒，避免频繁查询）
        static int dbRefreshCounter = 0;
        if (++dbRefreshCounter % 10 == 0 && m_pServer->sortingDb())
        {
            SortingStatistics stats = m_pServer->sortingDb()->statistics();
            m_lblDbStats->setText(QString("数据库: 总计 %1 条 | 今日 %2 条 | %3 波次 | %4 格口")
                .arg(stats.totalRecords)
                .arg(stats.todayRecords)
                .arg(stats.totalWaves)
                .arg(stats.totalGrids));
        }
    }
}

// ★ 2026-09-06 解耦：RFID 客户端连接状态 → UI 标签（状态变化时才改样式/记日志）
void MainWindow::updateRfidStatus()
{
    RfidPushClient* rfid = m_pServer ? m_pServer->rfidPush() : nullptr;
    bool now = (rfid != nullptr) && rfid->isConnected();
    if (m_rfidStatusInited && now == m_lastRfidConnected)
        return;
    m_rfidStatusInited = true;
    m_lastRfidConnected = now;

    if (!m_lblRfidStatus) return;
    if (now)
    {
        m_lblRfidStatus->setText(QString("● RFID: 已连接"));
        m_lblRfidStatus->setStyleSheet("font-size: 13px; color: #4CAF50; font-weight: bold;");
        m_lblRfidIp->setStyleSheet("font-size: 13px; color: #2196F3;");
        if (m_rfidStatusLog)
            appendLog(QString("[RFID] 推送连接已建立 %1").arg(m_lblRfidIp->text()));
        m_rfidStatusLog = true;
    }
    else
    {
        m_lblRfidStatus->setText(QString("RFID: 未连接(自动重连中)"));
        m_lblRfidStatus->setStyleSheet("font-size: 13px; color: #f44336; font-weight: bold;");
        m_lblRfidIp->setStyleSheet("font-size: 13px; color: #888;");
        if (m_rfidStatusLog)
            appendLog(QString("[RFID] 推送连接断开 %1（自动重连中，无需操作）").arg(m_lblRfidIp->text()), true);
    }
}

void MainWindow::updateWavePanel()
{
    WaveSnapshot snap = m_pServer->waveManager()->snapshot();

    m_lblWaveCode->setText(snap.orderCode.isEmpty() ? "-- 等待波次 --" : snap.orderCode);
    m_lblWaveStatus->setText(snap.statusText);
    m_lblSkuCount->setText(QString::number(snap.skuCount));
    m_lblSorted->setText(QString("%1 / %2").arg(snap.sortedCount).arg(snap.totalRecv));
    // ★ 分母含义提示：y=WMS下发的计划总件数 orderQty（H4 head.orderQty，strict校验=ΣgridNumber）
    m_lblSorted->setToolTip(QString::fromUtf8("已分拣件数 / 计划总件数（WMS下发 orderQty=计划件数）"));
    m_lblException->setText(QString::number(snap.exceptionCount));
    m_lblSumLocation->setText(QString::number(snap.sumLocation));
    // ★ 2026-09-07 效率显示：1分钟接收 RFID 推送件数 × 60 = 折算每小时件数（滑动窗口）
    {
        int perMin = m_pServer->rfidPushPerMinute();
        if (perMin > 0)
        {
            m_lblEfficiency->setText(QString("%1 件/时").arg(perMin * 60));
            m_lblEfficiency->setToolTip(QString::fromUtf8("最近1分钟接收 RFID 推送 %1 件，折算每小时 = %1 × 60 = %2 件/时")
                .arg(perMin).arg(perMin * 60));
        }
        else
        {
            m_lblEfficiency->setText(QString::fromUtf8("--"));
            m_lblEfficiency->setToolTip(QString::fromUtf8("开始接收 RFID 推送后显示效率（件/时，滑动1分钟窗口）"));
        }
    }
    // ★ 2026-09-07 峰值效率：当日最大（每分钟窗口件数峰值 × 60 折算件/时），每天最终值落库 daily_peak
    {
        int peakMin = m_pServer->peakPerMinuteToday();
        if (peakMin > 0)
        {
            m_lblPeakEff->setText(QString("%1 件/时").arg(peakMin * 60));
            m_lblPeakEff->setToolTip(QString::fromUtf8("当日峰值：1分钟窗口最高 %1 件 → 折算 %2 件/时（每天最终最大值保存到数据库 daily_peak）")
                .arg(peakMin).arg(peakMin * 60));
        }
        else
        {
            m_lblPeakEff->setText(QString::fromUtf8("--"));
            m_lblPeakEff->setToolTip(QString::fromUtf8("当日峰值效率（件/时，全天最大）"));
        }
    }
    m_lblLastWave->setText(snap.lastWaveCode.isEmpty() ? QString::fromUtf8("--") : snap.lastWaveCode);

    // 颜色提示
    if (snap.waveStatus == WAVE_SORTING)
        m_lblWaveStatus->setStyleSheet("font-size: 13px; font-weight: bold; color: #FF9800;");
    else if (snap.waveStatus >= WAVE_COMPLETING)
        m_lblWaveStatus->setStyleSheet("font-size: 13px; font-weight: bold; color: #4CAF50;");
    else
        m_lblWaveStatus->setStyleSheet("font-size: 13px; font-weight: bold; color: #2196F3;");

    // ★ 开始分拣按钮：接收中且 BOUND 或 SORTING 状态时橙色激活，否则灰色禁用
    //   （★ 2026-09-06 解耦：停止接收/等待完结时不可再开始分拣）
    bool canSort = m_bRunning
        && (snap.waveStatus == WAVE_BOUND || snap.waveStatus == WAVE_SORTING);
    m_btnStartSorting->setEnabled(canSort);
}

void MainWindow::updatePlcPanel()
{
    if (!m_pPlcMgr) return;

    PlcStats s = m_pPlcMgr->stats();

    // ══════════════════════════════════════════════════════════
    // TCP 连接状态
    // ══════════════════════════════════════════════════════════
    bool tcpNow = (s.running && s.clientCount > 0);
    if (tcpNow != m_lastTcpConnected)  // ★ 状态变化时才改样式
    {
        m_lastTcpConnected = tcpNow;
        if (tcpNow)
        {
            m_lblTcpStatus->setText(QString("● TCP: 已连接"));
            m_lblTcpStatus->setStyleSheet("font-size: 13px; color: #4CAF50; font-weight: bold;");
        }
        else if (s.running)
        {
            m_lblTcpStatus->setText(QString("● TCP: 监听中"));
            m_lblTcpStatus->setStyleSheet("font-size: 13px; color: #FF9800; font-weight: bold;");
        }
        else
        {
            m_lblTcpStatus->setText(QCoreApplication::translate("MainWindow", "TCP: 未启动"));
            m_lblTcpStatus->setStyleSheet("font-size: 13px; color: #f44336; font-weight: bold;");
        }
    }
    if (tcpNow || s.running)
    {
        m_lblTcpIp->setText(tcpNow ? QString("%1:%2").arg(s.lastIp.isEmpty() ? "?" : s.lastIp).arg(s.lastPort)
                                   : QString(":%1").arg(s.port));
    }

    // TCP 收发统计
    m_lblTcpSend->setText(QString("TCP发送: %1").arg(s.tcpSendCount));
    m_lblTcpSendErr->setText(QString("TCP失败: %1").arg(s.tcpSendErrCount));
    if (s.tcpSendErrCount > 0)
        m_lblTcpSendErr->setStyleSheet("font-size: 13px; color: #f44336; font-weight: bold;");
    else
        m_lblTcpSendErr->setStyleSheet("font-size: 13px; color: #888;");

    m_lblTcpRecv->setText(QString("TCP接收: %1").arg(s.recvCount));
    m_lblTcpRecv->setToolTip(QString("反馈计数: %1").arg(m_plcFeedbackCount.loadAcquire()));
    m_lblTcpConnCount->setText(QString("客户端: %1").arg(s.clientCount));

    // 运行时间
    if (s.uptimeSec > 0)
    {
        int h = (int)(s.uptimeSec / 3600);
        int m = (int)((s.uptimeSec % 3600) / 60);
        int sec = (int)(s.uptimeSec % 60);
        if (h > 0)
            m_lblTcpUptime->setText(QString("运行: %1h%2m%3s").arg(h).arg(m).arg(sec));
        else
            m_lblTcpUptime->setText(QString("运行: %1m%2s").arg(m).arg(sec));
    }
    else
    {
        m_lblTcpUptime->setText("");
    }

    // ══════════════════════════════════════════════════════════
    // S7 连接状态
    // ══════════════════════════════════════════════════════════
    if (s.s7Connected != m_lastS7Connected)  // ★ 状态变化时才改样式
    {
        m_lastS7Connected = s.s7Connected;
        if (s.s7Connected)
        {
            m_lblS7Status->setText(QString("● S7: 已连接"));
            m_lblS7Status->setStyleSheet("font-size: 13px; color: #4CAF50; font-weight: bold;");
            m_lblS7Ip->setText(s.s7Ip);
            m_lblS7Ip->setStyleSheet("font-size: 13px; color: #2196F3;");
        }
        else
        {
            m_lblS7Status->setText(QString("● S7: 未连接"));
            m_lblS7Status->setStyleSheet("font-size: 13px; color: #f44336; font-weight: bold;");
            m_lblS7Ip->setText(s.s7Ip.isEmpty() ? "" : s.s7Ip);
            m_lblS7Ip->setStyleSheet("font-size: 13px; color: #888;");
        }
    }

    // ★ 已注释：S7 DB1写入已移除，不再显示S7发送统计
    // m_lblS7Send->setText(QString("S7发送: %1").arg(s.s7SendCount));
    // m_lblS7SendErr->setText(QString("S7失败: %1").arg(s.s7SendErrCount));
    // if (s.s7SendErrCount > 0)
    //     m_lblS7SendErr->setStyleSheet("font-size: 13px; color: #f44336; font-weight: bold;");
    // else
    //     m_lblS7SendErr->setStyleSheet("font-size: 13px; color: #888;");

    // 锁格状态
    if (s.lockedGridCount > 0)
        m_lblS7LockGrids->setStyleSheet("font-size: 13px; color: #FF5722; font-weight: bold;");
    else
        m_lblS7LockGrids->setStyleSheet("font-size: 13px; color: #888;");
    m_lblS7LockGrids->setText(QString("锁格: %1").arg(s.lockedGridCount));

    // ══════════════════════════════════════════════════════════
    // 最近数据
    // ══════════════════════════════════════════════════════════
    if (!s.lastBarcode.isEmpty())
    {
        m_lblLastSendCode->setText(QString("最近发送: [%1]").arg(s.lastBarcode));
        m_lblLastSendCode->setStyleSheet("font-size: 12px; color: #333; font-weight: bold;");
        m_lblLastSendGrid->setText(QString("格口: %1 小车: %2").arg(s.lastGrid).arg(s.lastCar));
        m_lblLastSendGrid->setStyleSheet("font-size: 12px; color: #2196F3;");
        if (s.lastSendTimeMs > 0)
        {
            QDateTime dt = QDateTime::fromMSecsSinceEpoch(s.lastSendTimeMs);
            qint64 diff = QDateTime::currentMSecsSinceEpoch() - s.lastSendTimeMs;
            m_lblLastSendTime->setText(QString("发送: %1 (%2秒前)")
                .arg(dt.toString("HH:mm:ss")).arg(diff / 1000));
            m_lblLastSendTime->setStyleSheet("font-size: 12px; color: #888;");
        }
    }
    else
    {
        m_lblLastSendCode->setText(QCoreApplication::translate("MainWindow", "最近发送: --"));
        m_lblLastSendCode->setStyleSheet("font-size: 12px; color: #555;");
        m_lblLastSendGrid->setText("");
        m_lblLastSendTime->setText("");
    }

    if (!s.lastRecvCode.isEmpty())
    {
        m_lblLastRecvCode->setText(QString("最近接收: [%1]").arg(s.lastRecvCode));
        m_lblLastRecvCode->setStyleSheet("font-size: 12px; color: #333; font-weight: bold;");
        m_lblLastRecvGrid->setText(QString("格口: %1 小车: %2").arg(s.lastRecvGrid).arg(s.lastRecvCar));
        m_lblLastRecvGrid->setStyleSheet("font-size: 12px; color: #2196F3;");
    }
    else
    {
        m_lblLastRecvCode->setText(QCoreApplication::translate("MainWindow", "最近接收: --"));
        m_lblLastRecvCode->setStyleSheet("font-size: 12px; color: #555;");
        m_lblLastRecvGrid->setText("");
    }
    if (s.lastRecvTimeMs > 0)
    {
        QDateTime dt = QDateTime::fromMSecsSinceEpoch(s.lastRecvTimeMs);
        qint64 diff = QDateTime::currentMSecsSinceEpoch() - s.lastRecvTimeMs;
        m_lblLastRecvTime->setText(QString("接收: %1 (%2秒前)")
            .arg(dt.toString("HH:mm:ss")).arg(diff / 1000));
        m_lblLastRecvTime->setStyleSheet("font-size: 12px; color: #888;");
    }
    else
    {
        m_lblLastRecvTime->setText("");
    }
}

// ★ 实际执行「停止接收」收尾 —— 唯一停止出口（2026-09-02 起幂等；★ 2026-09-06 解耦后不再销毁实例）
//   调用方：H8 回传成功/耗尽（endReportFinished）、等待超时（安全网）、用户取消等待、程序退出
//   保证：任意路径到达都只会执行一次完整收尾；停止后软件保持运行、设备保持连接，
//         可再次点击"开始接收任务"
void MainWindow::doActualStop()
{
    // ★ 幂等：停止流程只执行一次（防 endReportFinished 与超时/取消重复触发）
    if (m_stopPhase == StopDone)
        return;
    m_stopPhase = StopDone;

    // ★ 停止超时安全网（正常流程已完成）
    if (m_stopTimeoutTimer) m_stopTimeoutTimer->stop();

    // ★ 2026-09-06 解耦：HttpServer/HttpClient 常驻（程序启动建一次，退出才析构），
    //   此处只停止「接收层」（HTTP 停止 + 停止兜底落库，幂等），
    //   设备层(PLC TCP/S7/RFID)保持连接，Outbox(H7/H8)补传定时器保持运行。
    //   —— 原"每轮启停 new/deleteLater 实例"方案取消：无实例泄漏、无启停竞态、信号只连一次。
    if (m_pServer)
        m_pServer->stopReceive();
    m_bRunning = false;   // ★ 确保状态复位（取消等待路径直接进入）

    // ★ 显示波次最终状态（H8 完结后状态已由 HttpServer 更新为完结/保持）
    if (m_pServer && m_pServer->waveManager())
        updateWavePanel();

    m_btnStartStop->setEnabled(true);
    m_btnStartStop->setText(QCoreApplication::translate("MainWindow", "开始接收任务"));
    m_btnStartStop->setStyleSheet(
        "QPushButton { background-color: #4CAF50; color: white; font-size: 14px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #45a049; }");
    m_lblServerStatus->setText(QCoreApplication::translate("MainWindow", "● 未接收任务"));
    m_lblServerStatus->setStyleSheet("font-size: 14px; color: #f44336;");

    appendLog("任务接收已停止（设备 PLC/RFID 保持连接；可再次点击「开始接收任务」）");
}


// ============================================================================
// 容器绑定状态
// ============================================================================

void MainWindow::onRefreshBindings()
{
    updateBindingPanel();
    appendLog("容器绑定状态已刷新");
}

// ★ 2026-09-07 清空格口容器绑定（人工重置按钮）
//   确认后：内存绑定清空 + DB 归档留史（可追溯/可沿用）+ 恢复满箱禁用格口 → 提示 + 日志
void MainWindow::onClearAllGridBinds()
{
    if (!m_pServer)
    {
        appendLog("[清空绑定] 服务未就绪", true);
        return;
    }

    auto ret = QMessageBox::question(this, QString::fromUtf8("清空格口容器绑定"),
        QString::fromUtf8("确定清空全部格口的当前容器绑定吗？\n\n"
                          " ① 所有格口恢复初始未绑定状态（可重新由 WMS 下发 H6 绑定）；\n"
                          " ② 原绑定记录将归档保留在数据库（可追溯、可沿用）；\n"
                          " ③ 满箱锁格禁用的格口一并恢复。"),
        QMessageBox::Yes | QMessageBox::Cancel);
    if (ret != QMessageBox::Yes)
        return;

    appendLog("[清空绑定] 执行清空格口容器绑定 ...", true);
    m_pServer->clearAllGridBinds();   // 内部：内存清空 + DB归档留史 + enableAllGrids + bindingUpdated + 日志
    updateBindingPanel();
    m_bindingDirty = true;
    appendLog("[清空绑定] 完成：格口已恢复初始状态，历史绑定已归档保留于数据库（可在日志/DB 追溯）", true);
    QMessageBox::information(this, QString::fromUtf8("已清空"),
        QString::fromUtf8("已清空全部格口容器绑定，格口恢复初始状态。\n历史绑定记录已归档保存在数据库中，可追溯。"));
}

void MainWindow::updateBindingPanel()
{
    if (!m_pServer) return;

    QMap<QString, QString> bindings = m_pServer->getContainerBindings();
    int boundCount = 0;
    int lockedCount = 0;          // ★ 2026-09-11：已锁格数量（黄色，PLC 物理锁格中/满箱锁格）
    int pendingRebindCount = 0;   // ★ 2026-09-09：已物理解锁但仍等待 WMS 重绑(H6)的格口数

    for (int i = 0; i < BINDING_SLOT_COUNT; ++i)
    {
        int gridNum = i + 1;
        QString gridKey = QString("%1").arg(gridNum, GRID_KEY_PADDING, 10, QChar('0'));
        QString boxCode = bindings.value(gridKey, "");

        QLabel* lblStatus = m_bindingLabels[i];
        QLabel* lblBox    = m_bindingBoxLabels[i];
        if (!lblStatus || !lblBox) continue;

        // ★ 2026-09-09 四态显示（客户现场：物理解锁后仍显示"锁格"）：
        //   ① 物理锁格中（S7 DB77 锁格位）           → 黄 "锁格"
        //   ② 已物理解锁但 WCS 仍禁用（等 H6 重绑）  → 橙 "已解锁·待重绑"
        //   ③ 未禁用且有容器绑定                     → 绿 箱号
        //   ④ 其余                                   → 灰 "未绑定"
        bool bPhysLocked = m_pPlcMgr && m_pPlcMgr->isGridLocked(gridNum);
        bool bDisabled   = m_pPlcMgr && m_pPlcMgr->isGridDisabled(gridNum);

        if (bPhysLocked)
        {
            boundCount++;
            lockedCount++;   // ★ 2026-09-11：黄色"锁格"格口计数
            lblStatus->setStyleSheet(
                "font-size: 10px; color: white; border-radius: 6px; background-color: #FFC107;");
            lblStatus->setToolTip(QString("格口%1 物理锁格中（黄色）：PLC 已锁定该格口（S7 锁格位置位）").arg(gridKey));
            lblBox->setText(boxCode.isEmpty() ? QString::fromUtf8("锁格") : boxCode);
            lblBox->setStyleSheet("font-size: 11px; color: #333; font-weight: bold; border: none; background: transparent;");
        }
        else if (bDisabled)
        {
            // ★ 现场已解锁，但满箱后尚未收到 WMS 重发 H6 绑定 → 暂不参与分配（等待重绑）
            boundCount++;
            pendingRebindCount++;
            lblStatus->setStyleSheet(
                "font-size: 10px; color: white; border-radius: 6px; background-color: #FF9800;");
            lblStatus->setToolTip(QString("格口%1 已解锁·待重绑（橙色）：现场已物理解锁；"
                                          "满箱后旧容器已归档，等待 WMS 重新下发容器绑定(H6)后恢复分配").arg(gridKey));
            lblBox->setText(boxCode.isEmpty() ? QString::fromUtf8("待重绑") : boxCode);
            lblBox->setStyleSheet("font-size: 11px; color: #333; font-weight: bold; border: none; background: transparent;");
        }
        else if (!boxCode.isEmpty())
        {
            boundCount++;
            lblStatus->setStyleSheet(
                "font-size: 10px; color: white; border-radius: 6px; background-color: #4CAF50;");
            lblStatus->setToolTip(QString("格口%1 ←→ %2 (已绑定)").arg(gridKey).arg(boxCode));
            lblBox->setText(boxCode);
            lblBox->setStyleSheet("font-size: 11px; color: #333; font-weight: bold; border: none; background: transparent;");
        }
        else
        {
            lblStatus->setStyleSheet(
                "font-size: 10px; color: white; border-radius: 6px; background-color: #bbb;");
            lblStatus->setToolTip(QString("格口%1: 未绑定").arg(gridKey));
            lblBox->setText("--");
            lblBox->setStyleSheet("font-size: 11px; color: #bbb; border: none; background: transparent;");
        }
    }

    int unboundCount = BINDING_SLOT_COUNT - boundCount;
    m_lblBoundCount->setText(QString("已绑定: %1").arg(boundCount));
    // ★ 2026-09-11 已锁格计数（黄色，紧跟"已绑定"）：PLC 物理锁格中（S7 锁格位=1，满箱锁格）
    //   口径：黄色"锁格"格口数；橙色"已解锁·待重绑"单独在 tooltip 中给出（未计入本数）
    if (m_lblLockedCount)
    {
        m_lblLockedCount->setText(QString::fromUtf8("已锁格: %1").arg(lockedCount));
        QString tip = QString::fromUtf8("已锁格: %1（黄色）——PLC 已锁定该格口（S7 锁格位置位/满箱），"
                                        "禁止继续分配与落格，等待 WMS 重发容器绑定(H6)后恢复")
                          .arg(lockedCount);
        if (pendingRebindCount > 0)
            tip += QString::fromUtf8("\n另有 %1 个格口为「已解锁·待重绑」（橙色）：已物理解锁，等待 WMS 重发 H6").arg(pendingRebindCount);
        m_lblLockedCount->setToolTip(tip);
    }
    m_lblUnboundCount->setText(QString("未绑定: %1").arg(unboundCount));
    // ★ 2026-09-09：待重绑格口数量提示（橙色格口，已解锁但等 H6 重绑）
    if (m_lblUnboundCount)
    {
        m_lblUnboundCount->setToolTip(pendingRebindCount > 0
            ? QString::fromUtf8("未绑定: %1（其中 %2 个为「已解锁·待重绑」：等待 WMS 重发 H6 容器绑定）")
                  .arg(unboundCount).arg(pendingRebindCount)
            : QString::fromUtf8("未绑定: %1").arg(unboundCount));
    }
}

// ============================================================================
// ★ 2026-09-06 波次数据记录面板（全部已传输波次 + 进度）——手动刷新
// ============================================================================

void MainWindow::onRefreshWaveRecords()
{
    if (!m_pServer || !m_tblWaveRecords) return;

    QVector<WaveRecordProgress> waves = m_pServer->getAllWaves();

    // ★ 2026-09-08：待执行队列快照（内存元数据，无 DB 开销、不含大报文）——用于状态列标注与按钮计数
    const QVector<HttpServer::PendingWaveInfo> pendingWaves = m_pServer->pendingWaves();
    if (m_btnViewWaveQueue)
        m_btnViewWaveQueue->setText(pendingWaves.isEmpty()
            ? QString::fromUtf8("查看接收波次队列")
            : QString::fromUtf8("查看接收波次队列(%1)").arg(pendingWaves.size()));

    // ★ 当前在内存中运行的波次：状态列显示实时状态（DB 状态可能滞后）
    WaveManager* wm = m_pServer->waveManager();
    QString liveOrder = wm ? wm->orderCode() : QString();
    int liveStatus    = wm ? wm->status() : -1;

    m_tblWaveRecords->setRowCount(waves.size());
    for (int row = 0; row < waves.size(); ++row)
    {
        const WaveRecordProgress& w = waves[row];

        // H7 满箱状态汇总
        QString h7Status = QString::fromUtf8("无");
        {
            QVector<OutboxRecord> fb = m_pServer->getWaveFullboxOutbox(w.orderCode);
            if (!fb.isEmpty())
            {
                int pend = 0, succ = 0, fail = 0;
                for (const OutboxRecord& r : fb)
                {
                    if (r.status == "success") ++succ;
                    else if (r.status == "failed") ++fail;
                    else ++pend;
                }
                h7Status = QString("成功%1/待发%2/失败%3").arg(succ).arg(pend).arg(fail);
            }
        }

        // H8 完结状态汇总
        QString h8Status = QString::fromUtf8("无");
        bool    h8Failed = false;   // ★ 2026-09-08 是否存在失败/已取消的完结报文（状态列标注用）
        {
            QVector<OutboxRecord> eb = m_pServer->getWaveEndOutbox(w.orderCode);
            if (!eb.isEmpty())
            {
                int pend = 0, succ = 0, fail = 0;
                for (const OutboxRecord& r : eb)
                {
                    if (r.status == "success") ++succ;
                    else if (r.status == "failed") ++fail;
                    else ++pend;
                }
                h8Status = QString("成功%1/待发%2/失败%3").arg(succ).arg(pend).arg(fail);
                h8Failed = (fail > 0);
            }
        }

        // 状态列：当前运行波次显示实时状态 + 「（当前）」标记
        // ★ 2026-09-08：处于待执行队列的波次显示「排队待执行」；H8 存在失败报文时追加「（回传失败·待重传）」
        QString statusText;
        bool inPendingQueue = false;
        for (const HttpServer::PendingWaveInfo& pw : pendingWaves)
        {
            if (pw.orderCode == w.orderCode) { inPendingQueue = true; break; }
        }
        if (!liveOrder.isEmpty() && w.orderCode == liveOrder)
            statusText = WaveSnapshot::statusToString(liveStatus) + QString::fromUtf8("（当前）");
        else if (inPendingQueue)
            statusText = QString::fromUtf8("已下发（排队待执行）");
        else
            statusText = WaveSnapshot::statusToString(w.status);

        if (h8Failed && w.status != WAVE_FINISHED)
            statusText += QString::fromUtf8("（回传失败·待重传）");

        auto setCell = [&](int col, const QString& text) {
            QTableWidgetItem* item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            m_tblWaveRecords->setItem(row, col, item);
        };
        // ★ 当前运行波次的已分拣/异常显示内存实时值
        int sortedCnt  = w.sortedCount;
        int excCnt     = w.exceptionCount;
        if (!liveOrder.isEmpty() && w.orderCode == liveOrder)
        {
            if (wm) { sortedCnt = wm->sorted(); excCnt = wm->exception(); }
        }
        setCell(0, w.orderCode);
        setCell(1, statusText);
        setCell(2, QString::number(w.orderQty));
        setCell(3, QString::number(sortedCnt));
        setCell(4, QString::number(excCnt));
        setCell(5, h7Status);
        setCell(6, h8Status);
        setCell(7, formatTimeFirst(w.updatedAt));   // ★ 2026-09-08 时间在前、年月在后
    }
}

// ★ 解析重传目标波次：列表选中行优先；未选中用当前内存波次（重传其未成功的 H7/H8）
QString MainWindow::selectedOrCurrentWaveOrder()
{
    if (m_tblWaveRecords)
    {
        int row = m_tblWaveRecords->currentRow();
        if (row >= 0)
        {
            QTableWidgetItem* it = m_tblWaveRecords->item(row, 0);
            if (it && !it->text().trimmed().isEmpty())
                return it->text().trimmed();
        }
    }
    if (m_pServer && m_pServer->waveManager())
    {
        WaveManager* wm = m_pServer->waveManager();
        if (wm->status() != WAVE_IDLE && !wm->orderCode().isEmpty())
            return wm->orderCode();
    }
    return QString();
}

void MainWindow::onResendSelectedH7()
{
    if (!m_pServer) return;

    // ★ 2026-09-08 UI需求2：H7 按钮读取「失败格口下拉」
    //   ① 选中失败记录 → 按该(波次,格口)精确重传失败/已取消的 H7 报文（不影响主流程）
    //   ② 未选中但手输了格口号 → 对当前波次该格口的分拣记录生成新的 H7 手动满箱上传
    //   ③ 下拉为"暂无失败记录"/空 → 沿用原行为（按选中波次/当前波次重发全部未成功 H7）
    if (m_cmbFailedH7)
    {
        const QString text = m_cmbFailedH7->currentText().trimmed();
        const bool noFailItem = text.isEmpty() || text == QString::fromUtf8("暂无失败记录");

        if (!noFailItem)
        {
            const int idx = m_cmbFailedH7->currentIndex();
            if (idx >= 0 && idx < m_failedH7Items.size() && m_cmbFailedH7->itemData(idx).isValid())
            {
                const HttpServer::FailedFullboxItem& item = m_failedH7Items.at(idx);
                appendLog(QString("[重传] 按失败格口精确补发 order=%1 grid=%2 报文数=%3（不影响主流程）")
                    .arg(item.orderCode).arg(item.grid).arg(item.failCount));
                m_pServer->resendFailedFullboxGrid(item.orderCode, item.grid);
                onRefreshWaveRecords();
                refreshFailedCombos();
                return;
            }

            // 手输格口（兼容 5 / 005 / 22005 三种写法）→ 手动满箱（生成新 H7 并上传）
            const QString grid = parseWmsGridCodeToStr(text);
            if (!grid.isEmpty())
            {
                appendLog(QString("[手动满箱] 格口%1 开始满箱切换上传 ...").arg(text));
                m_pServer->manualFullbox(grid);
                onRefreshWaveRecords();
                return;
            }
            appendLog(QString("[重传] 格口号无法识别：%1（支持 5 / 005 / 22005 形式）").arg(text), true);
            return;
        }
    }

    QString orderCode = selectedOrCurrentWaveOrder();
    if (orderCode.isEmpty())
    {
        appendLog("[重传] 未指定波次：请在「波次数据记录」中选中一行（或当前有运行波次）", true);
        return;
    }
    appendLog(QString("[重传] 满箱切换(H7) 主动补发 order=%1（不影响主工作流）").arg(orderCode));
    m_pServer->resendOutbox(orderCode, true, false);
    onRefreshWaveRecords();
}

void MainWindow::onResendSelectedH8()
{
    if (!m_pServer) return;

    // ★ 2026-09-08 UI需求3：H8 按钮优先读取「失败波次下拉」——选中后只重传该波次的失败 H8
    if (m_cmbFailedH8)
    {
        const int idx = m_cmbFailedH8->currentIndex();
        if (idx >= 0 && idx < m_failedH8Items.size() && m_cmbFailedH8->itemData(idx).isValid())
        {
            const HttpServer::FailedEndItem& item = m_failedH8Items.at(idx);
            appendLog(QString("[重传] 按失败波次精确补发 order=%1 报文数=%2（不影响主流程）")
                .arg(item.orderCode).arg(item.failCount));
            m_pServer->resendFailedEnd(item.orderCode);
            onRefreshWaveRecords();
            refreshFailedCombos();
            return;
        }
    }

    QString orderCode = selectedOrCurrentWaveOrder();
    if (orderCode.isEmpty())
    {
        appendLog("[重传] 未指定波次：请在「波次数据记录」中选中一行（或当前有运行波次）", true);
        return;
    }
    appendLog(QString("[重传] 任务完结(H8) 主动补发 order=%1（不影响主工作流）").arg(orderCode));
    m_pServer->resendOutbox(orderCode, false, true);
    onRefreshWaveRecords();
}

// ============================================================================
// ★ 2026-09-08 UI需求2/3：刷新「H7 失败格口 / H8 失败波次」两个下拉
//   数据源 = outbox 表中 status IN ('failed','cancelled') 的历史报文（上限200条）
//   无记录时显示「暂无失败记录」（灰色提示项），并清空可编辑框方便直接手输格口
// ============================================================================
void MainWindow::refreshFailedCombos()
{
    if (!m_pServer) return;

    // ── H7 失败格口下拉 ──
    if (m_cmbFailedH7)
    {
        const QString keepText = m_cmbFailedH7->currentText().trimmed();
        {
            QSignalBlocker blocker(m_cmbFailedH7);
            m_cmbFailedH7->clear();
            m_failedH7Items = m_pServer->getFailedFullboxItems(200);
            for (const HttpServer::FailedFullboxItem& it : m_failedH7Items)
            {
                const QString st = (it.status == "failed")    ? QString::fromUtf8("失败")
                                 : (it.status == "cancelled") ? QString::fromUtf8("已取消重试")
                                                              : QString::fromUtf8("失败/已取消");
                m_cmbFailedH7->addItem(QString::fromUtf8("格口%1 · 波次%2 · %3%4条")
                        .arg(it.grid).arg(it.orderCode).arg(st).arg(it.failCount),
                    it.orderCode + "|" + it.grid);
            }
            if (m_cmbFailedH7->count() == 0)
            {
                m_failedH7Items.clear();
                m_cmbFailedH7->addItem(QString::fromUtf8("暂无失败记录"));
                m_cmbFailedH7->setCurrentIndex(0);
                if (m_cmbFailedH7->lineEdit()) m_cmbFailedH7->lineEdit()->clear();   // 便于直接手输格口
            }
            else
            {
                int k = m_cmbFailedH7->findText(keepText);
                m_cmbFailedH7->setCurrentIndex(k >= 0 ? k : 0);
            }
        }
    }

    // ── H8 失败波次下拉 ──
    if (m_cmbFailedH8)
    {
        const QString keepText = m_cmbFailedH8->currentText().trimmed();
        {
            QSignalBlocker blocker(m_cmbFailedH8);
            m_cmbFailedH8->clear();
            m_failedH8Items = m_pServer->getFailedEndItems(200);
            for (const HttpServer::FailedEndItem& it : m_failedH8Items)
            {
                const QString st = (it.status == "failed")    ? QString::fromUtf8("失败")
                                 : (it.status == "cancelled") ? QString::fromUtf8("已取消重试")
                                                              : QString::fromUtf8("失败/已取消");
                m_cmbFailedH8->addItem(QString::fromUtf8("波次%1 · %2%3条")
                        .arg(it.orderCode).arg(st).arg(it.failCount),
                    it.orderCode);
            }
            if (m_cmbFailedH8->count() == 0)
            {
                m_failedH8Items.clear();
                m_cmbFailedH8->addItem(QString::fromUtf8("暂无失败记录"));
                m_cmbFailedH8->setCurrentIndex(0);
            }
            else
            {
                int k = m_cmbFailedH8->findText(keepText);
                m_cmbFailedH8->setCurrentIndex(k >= 0 ? k : 0);
            }
        }
    }
}

// ============================================================================
// ★ 2026-09-08 UI需求6：「波次数据记录」更新时间显示
//   DB 存的是 "yyyy-MM-dd HH:mm:ss[.zzz]" → 显示为 "HH:mm:ss yyyy-MM-dd"（时间在前、年月在后）
//   解析失败时原样返回（不隐藏问题数据）
// ============================================================================
QString MainWindow::formatTimeFirst(const QString& dbTime)
{
    const QString t = dbTime.trimmed();
    if (t.isEmpty()) return t;

    QDateTime dt = QDateTime::fromString(t, "yyyy-MM-dd HH:mm:ss");
    if (!dt.isValid()) dt = QDateTime::fromString(t, "yyyy-MM-dd HH:mm:ss.zzz");
    if (!dt.isValid()) dt = QDateTime::fromString(t.left(19), "yyyy-MM-dd HH:mm:ss");
    if (!dt.isValid()) return t;

    return dt.toString("HH:mm:ss yyyy-MM-dd");
}

// ============================================================================
// ★ 2026-09-08 UI需求7：查看接收波次队列（弹窗）
//   列表里同时放两样东西：
//     ① 首行操作项「接收新任务」——点击即**不处理排队波次，直接开始新任务**
//        （当前波次仍在作业中时拒绝并提示先点「结束任务」，避免打断现场分拣）
//     ② 其余行 = 剩余待执行波次（FIFO：序号/波次号/件数/接收时间）
//   队列执行入口说明：当前波次结束后自动开始 / 点「开始接收任务」时执行队首 /
//   在「波次数据记录」中用「切换选中波次」立即接管
// ============================================================================
void MainWindow::onViewWaveQueue()
{
    if (!m_pServer) return;

    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8("接收波次队列（剩余待执行波次）"));
    dlg.resize(780, 440);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);

    QLabel* tip = new QLabel(QString::fromUtf8(
        "列表首行「接收新任务」= 不处理排队波次，直接开始新任务（切出当前波次并等待 WMS 下发新波次）。\n"
        "排队波次的执行：当前波次结束后自动开始；点「开始接收任务」时自动执行队首；"
        "也可在「波次数据记录」中用「切换选中波次」立即接管。"), &dlg);
    tip->setStyleSheet("font-size: 12px; color: #555;");
    tip->setWordWrap(true);
    lay->addWidget(tip);

    QTableWidget* tbl = new QTableWidget(&dlg);
    tbl->setColumnCount(4);
    tbl->setHorizontalHeaderLabels(QStringList()
        << QString::fromUtf8("操作 / 序号") << QString::fromUtf8("波次号")
        << QString::fromUtf8("件数") << QString::fromUtf8("接收时间"));
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    tbl->setSelectionMode(QAbstractItemView::SingleSelection);
    tbl->verticalHeader()->setVisible(false);
    tbl->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tbl->horizontalHeader()->setStretchLastSection(true);
    tbl->setMinimumHeight(250);
    tbl->setStyleSheet(
        "QTableWidget { font-size: 12px; }"
        "QHeaderView::section { background-color: #e0e0e0; font-weight: bold; padding: 4px; }");
    lay->addWidget(tbl);

    // 当前波次是否处于作业态（作业中不允许直接开始新任务——按现场要求先点「结束任务」）
    auto isWaveWorking = [this](QString* statusText) -> bool {
        if (!m_pServer || !m_pServer->waveManager()) return false;
        int st = m_pServer->waveManager()->status();
        bool working = (st == WAVE_CREATED || st == WAVE_BOUND ||
                        st == WAVE_SORTING || st == WAVE_FULLBOX_SYNC);
        if (statusText) *statusText = WaveSnapshot::statusToString(st);
        return working;
    };

    bool startNewTask = false;   // 「接收新任务」被点击（关闭弹窗后执行，避免在弹窗回调里切 UI）

    auto rebuild = [this, tbl, &startNewTask, &dlg, isWaveWorking]() {
        const QVector<HttpServer::PendingWaveInfo> list =
            m_pServer ? m_pServer->pendingWaves() : QVector<HttpServer::PendingWaveInfo>();

        tbl->setRowCount(0);

        // ── 首行：操作项「接收新任务」（与剩余波次放在同一个列表里）──
        tbl->insertRow(0);
        {
            QPushButton* btnNew = new QPushButton(QString::fromUtf8("接收新任务"), tbl);
            btnNew->setMinimumHeight(26);
            btnNew->setStyleSheet(
                "QPushButton { background-color: #FF5722; color: white; font-size: 12px; font-weight: bold; "
                "border-radius: 4px; padding: 3px 10px; }"
                "QPushButton:hover { background-color: #E64A19; }");
            connect(btnNew, &QPushButton::clicked, &dlg, [this, &startNewTask, &dlg, isWaveWorking]() {
                QString stText;
                if (isWaveWorking(&stText))
                {
                    QMessageBox::information(this, QString::fromUtf8("接收新任务"),
                        QString::fromUtf8("当前波次仍在作业中（%1）。\n\n请先点击「结束任务」，"
                                          "再开始新任务。").arg(stText));
                    return;
                }
                startNewTask = true;
                dlg.accept();
            });
            tbl->setCellWidget(0, 0, btnNew);

            QTableWidgetItem* hint = new QTableWidgetItem(QString::fromUtf8(
                "不处理排队波次，直接开始新任务（等待 WMS 下发新波次）"));
            hint->setForeground(QColor("#FF5722"));
            tbl->setItem(0, 1, hint);
            tbl->setItem(0, 2, new QTableWidgetItem("--"));
            tbl->setItem(0, 3, new QTableWidgetItem("--"));
        }

        // ── 其余行：剩余待执行波次（FIFO）──
        int no = 0;
        for (const HttpServer::PendingWaveInfo& pw : list)
        {
            const int r = tbl->rowCount();
            tbl->insertRow(r);
            ++no;
            tbl->setItem(r, 0, new QTableWidgetItem(QString::number(no)));
            tbl->setItem(r, 1, new QTableWidgetItem(pw.orderCode));
            tbl->setItem(r, 2, new QTableWidgetItem(QString::number(pw.orderQty)));
            tbl->setItem(r, 3, new QTableWidgetItem(
                QDateTime::fromMSecsSinceEpoch(pw.recvTime).toString("HH:mm:ss yyyy-MM-dd")));
        }
        if (list.isEmpty())
        {
            const int r = tbl->rowCount();
            tbl->insertRow(r);
            QTableWidgetItem* empty = new QTableWidgetItem(
                QString::fromUtf8("（当前没有待执行波次——若 WMS 下发新波次而当前波次仍在执行，会自动排队显示在这里）"));
            empty->setForeground(QColor("#888"));
            tbl->setItem(r, 0, empty);
            tbl->setSpan(r, 0, 1, 4);
        }
    };

    // 队列变化时自动刷新（弹窗打开期间）
    connect(m_pServer, &HttpServer::pendingWavesChanged, &dlg, [rebuild]() { rebuild(); });

    rebuild();

    // ── 底部：刷新 / 关闭 ──
    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    QPushButton* btnRefresh = new QPushButton(QString::fromUtf8("刷新"), &dlg);
    QPushButton* btnClose   = new QPushButton(QString::fromUtf8("关闭"), &dlg);
    connect(btnRefresh, &QPushButton::clicked, &dlg, [rebuild]() { rebuild(); });
    connect(btnClose,   &QPushButton::clicked, &dlg, [&dlg]() { dlg.accept(); });
    btnRow->addWidget(btnRefresh);
    btnRow->addWidget(btnClose);
    lay->addLayout(btnRow);

    dlg.exec();

    // ★ 点击「接收新任务」→ 弹窗已关闭，此处执行（复用「新任务」既有确认与 UI 复位流程；队列原样保留）
    if (startNewTask)
        onStartNewWaveTask();
}

// ★ 切换选中波次：未完成→按 DB 进度恢复到内存继续；已完成/已取消→载入查看
//   ★ 2026-09-06 状态隔离：任意状态都可切出；若正在等待 H8 完结，自动取消等待再切换（报文留 outbox 补发）
void MainWindow::onResumeSelectedWave()
{
    if (m_stopPhase == StopEnding)
    {
        appendLog("[切换] 正在等待完结回传(H8)——自动取消等待（H8报文保留outbox继续补发），继续切换", true);
        doActualStop();   // 幂等收尾：停止接收 + UI 复位；H8 未确认报文保留
    }
    if (!m_pServer || !m_tblWaveRecords) return;

    int row = m_tblWaveRecords->currentRow();
    if (row < 0)
    {
        appendLog("[切换] 请先在「波次数据记录」中选中一行波次", true);
        return;
    }

    QTableWidgetItem* it = m_tblWaveRecords->item(row, 0);
    if (!it) return;
    QString orderCode = it->text().trimmed();
    if (orderCode.isEmpty()) return;

    // 摘要弹窗（含数据快照）
    QJsonObject s = m_pServer->getUnfinishedWaveSummary(orderCode);
    if (s.isEmpty())
    {
        appendLog(QString("[切换] 获取波次摘要失败 order=%1").arg(orderCode), true);
        return;
    }

    int dbStatus = s["status"].toInt();
    bool bViewOnly = (dbStatus == WAVE_FINISHED || dbStatus == WAVE_CANCELLED);
    QString msg = QString(
        "波次号: %1\n"
        "状态: %2\n"
        "计划件数: %3\n"
        "已分拣: %4    异常: %5\n"
        "H7满箱: %6\n"
        "H8完结: %7\n"
        "更新时间: %8\n\n"
        "%9")
        .arg(s["orderCode"].toString())
        .arg(s["statusText"].toString())
        .arg(s["orderQty"].toInt())
        .arg(s["sortedCount"].toInt())
        .arg(s["exceptionCount"].toInt())
        .arg(s["h7"].toString())
        .arg(s["h8"].toString())
        .arg(s["updatedAt"].toString())
        .arg(bViewOnly
             ? QString::fromUtf8("该波次已完结/取消：将以「查看模式」载入其数据\n（不参与分拣/回传；可重传H7/H8核对）")
             : QString::fromUtf8("切换到该波次并按其上次进度继续？\n（切换后若当前有其它任务会先保留其进度与数据）"));

    QMessageBox box(QMessageBox::Question, QString::fromUtf8("切换波次"), msg,
                    QMessageBox::Yes | QMessageBox::Cancel, this);
    box.button(QMessageBox::Yes)->setText(bViewOnly ? QString::fromUtf8("载入查看") : QString::fromUtf8("切换到该波次"));
    box.button(QMessageBox::Cancel)->setText(QString::fromUtf8("取消"));
    if (box.exec() != QMessageBox::Yes)
        return;

    appendLog(QString("[切换] 执行切换 order=%1 ...").arg(orderCode));
    bool ok = m_pServer->resumeUnfinishedWave(orderCode);
    if (ok)
    {
        int status = m_pServer->waveManager() ? m_pServer->waveManager()->status() : -1;
        if (status == WAVE_CREATED || status == WAVE_BOUND)
        {
            QString hint;
            if (!m_bRunning)
                hint = QString::fromUtf8("当前未开启任务接收，请先点击「开始接收任务」，再点击「开始分拣」继续。");
            else if (dbStatus == WAVE_HELD || dbStatus == WAVE_CANCEL_PENDING)
                hint = QString::fromUtf8("原状态：异常挂起。已切换到已绑定——\n请点击「开始分拣」，分拣完成后点「结束任务」重新回传完结（生成新H8），成功后任务即完结。");
            else if (status == WAVE_BOUND)
                hint = QString::fromUtf8("当前状态：已绑定，请点击「开始分拣」继续。");
            else
                hint = QString::fromUtf8("当前状态：已下发，请等待 WMS 下发容器绑定（H6）推进到已绑定后，再点击「开始分拣」。");
            QMessageBox::information(this, QString::fromUtf8("已切换"),
                QString::fromUtf8("已切换波次：%1\n%2").arg(orderCode).arg(hint));
        }
        else if (status == WAVE_FINISHED || status == WAVE_CANCELLED)
        {
            // ★ 历史终态波次「载入查看」
            QMessageBox::information(this, QString::fromUtf8("已载入"),
                QString::fromUtf8("已载入历史波次（查看模式）：%1（%2）\n"
                                  "当前为查看态，不参与分拣/回传；可查看数据，或点击「重传满箱切换/重传任务完结」补发核对。")
                    .arg(orderCode).arg(WaveSnapshot::statusToString(status)));
        }
        updateWavePanel();
        updateBindingPanel();
        onRefreshWaveRecords();
    }
    else
    {
        appendLog(QString("[切换] 切换失败 order=%1（详见上方原因）").arg(orderCode), true);
    }
}

// ★ 新任务：当前波次的进度与全部数据保留（DB），内存清空回到空闲；
//   之后 WMS 下发新波次即开始新任务；旧波次可从列表「切换」回来继续
//   ★ 2026-09-06 状态隔离：若正在等待 H8 完结，自动取消等待再切出
void MainWindow::onStartNewWaveTask()
{
    if (m_stopPhase == StopEnding)
    {
        appendLog("[新任务] 正在等待完结回传(H8)——自动取消等待（H8报文保留outbox继续补发），继续开始新任务", true);
        doActualStop();
    }
    if (!m_pServer || !m_pServer->waveManager())
    {
        appendLog("[新任务] 服务未就绪", true);
        return;
    }

    WaveManager* wm = m_pServer->waveManager();
    QString curOrder = wm->orderCode();
    int curStatus    = wm->status();

    bool hasActiveWave = !(curOrder.isEmpty() || curStatus == WAVE_IDLE);

    if (hasActiveWave)
    {
        auto ret = QMessageBox::question(this, QString::fromUtf8("开始新任务"),
            QString("当前波次：%1（%2）\n\n"
                    "点击「开始新任务」后：\n"
                    "  ① 当前波次进度与全部数据保留（可从「波次数据记录」切换回来继续）；\n"
                    "  ② 界面回到空闲、容器绑定与格口状态复位为初始全新状态，等待 WMS 下发新波次；\n"
                    "  ③ 未成功的 H7/H8 回传可在切回该波次时自动补发，或用「重传」按钮。\n\n"
                    "确定开始新任务吗？")
                .arg(curOrder).arg(WaveSnapshot::statusToString(curStatus)),
            QMessageBox::Yes | QMessageBox::Cancel);
        if (ret != QMessageBox::Yes)
            return;
        appendLog(QString("[新任务] 开始新任务，当前波次进度已保留 order=%1（%2）").arg(curOrder).arg(WaveSnapshot::statusToString(curStatus)));
    }
    else
    {
        appendLog("[新任务] 当前无任务，执行初始状态复位——等待 WMS 下发新波次（需已开启任务接收）");
    }

    m_pServer->startNewWaveTask();
    updateWavePanel();
    updateBindingPanel();
    onRefreshWaveRecords();
    // ★ 2026-09-08 需求7a：「新任务」不读取/执行剩余队列——队列原样保留，仅回到空闲等 WMS 下发新波次
    const int pendingCnt = m_pServer->pendingWaveCount();
    if (pendingCnt > 0)
        appendLog(QString("[新任务] 已回到空闲：等待 WMS 下发新波次；剩余待执行波次 %1 个已保留"
                          "（不自动执行，可点「查看接收波次队列」查看）").arg(pendingCnt));
    else
        appendLog("[新任务] 已回到空闲（初始全新状态）：等待 WMS 下发新波次；旧波次可随时从「波次数据记录」切换回来");
}

void MainWindow::onClearLog()
{
    m_txtLog->clear();
    // 同时清空缓冲队列
    QMutexLocker locker(&m_logMutex);
    m_logBuffer.clear();
}

void MainWindow::appendLog(const QString& msg, bool isError)
{
    // ★ 缓冲到队列，由定时器批量刷新到 UI，避免高频场景下 QTextEdit::append 卡死界面
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    QString color = isError ? "#f44336" : "#212121";
    QString text = QString("<span style='color:#888;'>[%1]</span> "
                           "<span style='color:%2;'>%3</span>")
                       .arg(timestamp).arg(color).arg(msg.toHtmlEscaped());

    {
        QMutexLocker locker(&m_logMutex);
        m_logBuffer.append(text);
    }

    // 同时写入log_center（备份日志，不受缓冲影响）
    if (isError)
        LogCenter::Instance()->wcs_run_log_warn(false, msg);
    else
        LogCenter::Instance()->wcs_run_log_warn(true, msg);
}

// ============================================================================
// flushLogBuffer — 定时批量刷新日志缓冲到 UI（动态降频）
//
// 正常: 每 100ms 刷新，单次最多 100 条
// 积压>50: 降频到 200ms，单次最多 50 条
// 积压>200: 降频到 500ms，丢弃非错误日志，单次最多 20 条
// 积压>500: 降频到 1000ms，只保留错误日志
// ============================================================================
void MainWindow::flushLogBuffer()
{
    // 批量取出缓冲队列
    QStringList batch;
    int queueSize = 0;
    {
        QMutexLocker locker(&m_logMutex);
        queueSize = m_logBuffer.size();
        if (queueSize == 0) return;
        batch.swap(m_logBuffer);
    }

    // ★ 动态调整刷新间隔
    int newInterval = LOG_FLUSH_INTERVAL_MS;
    if (queueSize > 500)      newInterval = 1000;
    else if (queueSize > 200) newInterval = 500;
    else if (queueSize > 50)  newInterval = 200;

    if (newInterval != m_logFlushIntervalMs)
    {
        m_logFlushIntervalMs = newInterval;
        m_logFlushTimer->setInterval(newInterval);
    }

    // ★ 高负载时丢弃非错误日志
    if (queueSize > 200)
    {
        QStringList filtered;
        for (const QString& line : batch)
        {
            if (line.contains("#f44336"))  // 红色=错误日志
                filtered.append(line);
        }
        m_logDropCount += (batch.size() - filtered.size());
        batch = filtered;
        if (batch.isEmpty()) return;
    }

    // ★ 截断保护：根据负载动态调整单次刷新上限
    int maxBatch = LOG_FLUSH_MAX_BATCH_SIZE;
    if (queueSize > 500)      maxBatch = 20;
    else if (queueSize > 200) maxBatch = 50;

    if (batch.size() > maxBatch)
    {
        QStringList remaining = batch.mid(maxBatch);
        {
            QMutexLocker locker(&m_logMutex);
            for (int i = remaining.size() - 1; i >= 0; --i)
                m_logBuffer.prepend(remaining[i]);
        }
        batch = batch.mid(0, maxBatch);
    }

    // 批量写入 UI
    m_txtLog->setUpdatesEnabled(false);
    for (const QString& line : batch)
        m_txtLog->append(line);
    m_txtLog->setUpdatesEnabled(true);

    m_txtLog->moveCursor(QTextCursor::End);
}

// ============================================================================
// ★ 2026-09-07 onOpenEffChart — 打开 RFID 推送效率统计弹窗
//   单实例复用：已打开则置前；关闭即销毁（WA_DeleteOnClose），仅当日观察不落盘
// ============================================================================
void MainWindow::onOpenEffChart()
{
    if (!m_pServer) return;

    if (m_effDlg)
    {
        m_effDlg->show();
        m_effDlg->raise();
        m_effDlg->activateWindow();
        return;
    }

    m_effDlg = new EfficiencyChartDialog(m_pServer, this);
    m_effDlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_effDlg, &QDialog::destroyed, this, [this]() { m_effDlg = nullptr; });
    m_effDlg->show();
}

// ============================================================================
// ★ onQueryRecords — 分拣记录查询（按EPC / 按SKU查格口）
// ============================================================================

void MainWindow::onQueryRecords()
{
    SortingDatabase* db = m_pQueryDb;
    if (!db->isOpen())
    {
        // ★ 重试打开（构造函数执行时 data 目录可能尚未创建，首次查询时补开）
        QString dbPath = QCoreApplication::applicationDirPath() + "/" + SORTING_DB_FILE;
        if (!db->open(dbPath))
        {
            appendLog("[查询] 数据库未就绪，请检查 data/sorting_records.db 是否存在", true);
            return;
        }
    }

    int queryMode = m_cmbQueryMode->currentIndex();  // 0=按EPC查询, 1=按SKU查询格口, 2=按格口查询

    if (queryMode == 2)
    {
        // ★ 2026-09-09 需求2：按格口查询分拣数量（留空=全格口汇总，输入格口号=该格明细）
        QString grid = m_editQueryBarcode->text().trimmed();

        if (grid.isEmpty())
        {
            // ── 全格口汇总：每格一行（格口号/分拣数量/SKU数/容器号/最近分拣时间）──
            QVector<GridSummaryRecord> sums = db->queryGridSummary();
            m_tblRecords->setColumnCount(6);
            m_tblRecords->setHorizontalHeaderLabels({
                QString::fromUtf8("序号"),
                QString::fromUtf8("格口号"),
                QString::fromUtf8("分拣数量"),
                QString::fromUtf8("SKU数"),
                QString::fromUtf8("容器号"),
                QString::fromUtf8("最近分拣时间")
            });
            m_tblRecords->setRowCount(0);
            m_tblRecords->setRowCount(sums.size());
            int totalItems = 0;
            for (int i = 0; i < sums.size(); ++i)
            {
                const GridSummaryRecord& g = sums[i];
                totalItems += g.sortedCount;
                auto* c0 = new QTableWidgetItem(QString::number(i + 1));
                c0->setTextAlignment(Qt::AlignCenter);
                m_tblRecords->setItem(i, 0, c0);
                m_tblRecords->setItem(i, 1, new QTableWidgetItem(g.gridNum));
                auto* c2 = new QTableWidgetItem(QString::number(g.sortedCount));
                c2->setTextAlignment(Qt::AlignCenter);
                m_tblRecords->setItem(i, 2, c2);
                auto* c3 = new QTableWidgetItem(QString::number(g.skuCount));
                c3->setTextAlignment(Qt::AlignCenter);
                m_tblRecords->setItem(i, 3, c3);
                m_tblRecords->setItem(i, 4, new QTableWidgetItem(g.boxcode));
                m_tblRecords->setItem(i, 5, new QTableWidgetItem(g.lastSortTime));
            }
            m_lblRecordCount->setStyleSheet("font-size: 12px; color: #555;");
            m_lblRecordCount->setText(QString::fromUtf8("全格口汇总：%1 个格口，共 %2 件")
                .arg(sums.size()).arg(totalItems));
            appendLog(QString::fromUtf8("[查询] 按格口汇总：%1 个格口，共 %2 件").arg(sums.size()).arg(totalItems));
        }
        else
        {
            // ── 该格分拣明细 ──
            // ★ 2026-09-10/11 查询兼容：输入 "7"（裸数字）/ "007"（补零）/ "22007"（WMS编码）等写法，
            //   统一在内部归一成同一个内部格口 key 后再查询——UI 表现与结果完全一致（无差异）
            const QString gridKey = gridKeyOf(grid);
            const QString wmsCode = gridToWmsCode(gridKey);   // 内部 key → WMS 编码（"007" → "22007"）

            QVector<SortingRecord> recs = db->queryByGrid(gridKey, SORTING_QUERY_MAX_RESULTS);
            m_tblRecords->setColumnCount(10);
            m_tblRecords->setHorizontalHeaderLabels({
                QString::fromUtf8("序号"),
                QString::fromUtf8("波次号"),
                QString::fromUtf8("EPC编码"),
                QString::fromUtf8("SKU编码"),
                QString::fromUtf8("格口号"),
                QString::fromUtf8("容器号"),
                QString::fromUtf8("件数"),
                QString::fromUtf8("库位"),
                QString::fromUtf8("分拣时间"),
                QString::fromUtf8("状态")
            });
            m_tblRecords->setRowCount(0);
            m_tblRecords->setRowCount(recs.size());
            for (int i = 0; i < recs.size(); ++i)
            {
                const SortingRecord& rec = recs[i];
                auto* c0 = new QTableWidgetItem(QString::number(i + 1));
                c0->setTextAlignment(Qt::AlignCenter);
                m_tblRecords->setItem(i, 0, c0);
                m_tblRecords->setItem(i, 1, new QTableWidgetItem(rec.orderCode));
                m_tblRecords->setItem(i, 2, new QTableWidgetItem(rec.barcode));
                m_tblRecords->setItem(i, 3, new QTableWidgetItem(rec.sku));
                // 格口号统一按内部 3 位 key 显示（历史脏数据原样显示）
                auto* gridItem = new QTableWidgetItem(gridKeyOf(rec.gridNum));
                gridItem->setToolTip(QString::fromUtf8("WMS编码: %1").arg(gridToWmsCode(gridKeyOf(rec.gridNum))));
                m_tblRecords->setItem(i, 4, gridItem);
                m_tblRecords->setItem(i, 5, new QTableWidgetItem(rec.boxcode));
                auto* c6 = new QTableWidgetItem(QString::number(rec.gridCount));
                c6->setTextAlignment(Qt::AlignCenter);
                m_tblRecords->setItem(i, 6, c6);
                m_tblRecords->setItem(i, 7, new QTableWidgetItem(rec.volu));
                m_tblRecords->setItem(i, 8, new QTableWidgetItem(rec.sortTime));
                auto* statusItem = new QTableWidgetItem(QString::fromUtf8("已分拣"));
                statusItem->setTextAlignment(Qt::AlignCenter);
                statusItem->setForeground(QColor("#228B22"));
                m_tblRecords->setItem(i, 9, statusItem);
            }
            m_lblRecordCount->setStyleSheet("font-size: 12px; color: #555;");
            m_lblRecordCount->setText(QString::fromUtf8("格口 [%1]（WMS编码 %2）分拣数量：%3 件")
                .arg(gridKey).arg(wmsCode).arg(recs.size()));
            // ★ 2026-09-11：日志不体现原始输入写法（"7"/"007"/"22007" 完全一致，UI 无差异）
            appendLog(QString::fromUtf8("[查询] 格口 [%1]（WMS编码 %2）分拣数量：%3 件")
                .arg(gridKey).arg(wmsCode).arg(recs.size()));
            if (recs.isEmpty())
            {
                appendLog(QString::fromUtf8("[查询] 格口 [%1] 无分拣记录")
                    .arg(gridKey), true);
            }
        }

        // 更新数据库统计
        SortingStatistics stats = db->statistics();
        m_lblDbStats->setText(QString::fromUtf8("数据库: 总计 %1 条 | 今日 %2 条 | %3 波次 | %4 格口")
            .arg(stats.totalRecords)
            .arg(stats.todayRecords)
            .arg(stats.totalWaves)
            .arg(stats.totalGrids));
        return;
    }

    if (queryMode == 1)
    {
        // ★ 按 SKU 查询格口分配
        QString sku = m_editQuerySku->text().trimmed();
        if (sku.isEmpty())
        {
            appendLog("[查询] 请输入 SKU 编码", true);
            return;
        }

        // ★ 2026-09-10 需求1：按 SKU 查询格口分配 + 该 SKU 下所有 EPC 及其实际落格号
        //   ① querySkuGridMapping：WMS 下发的计划格口（计划视角）
        //   ② queryBySku：sorting_records 落格明细，每条=1 个 EPC，grid_num 即实际落格号（实绩视角）
        //   一行 = 一个 EPC ↔ 其实际落格号（无落格的计划格口单独出一行"待分拣"）
        QVector<ReturnWaveItemRecord> items = db->querySkuGridMapping(sku);
        QVector<SortingRecord> details = db->queryBySku(sku, SORTING_QUERY_MAX_RESULTS);

        // 波次 → 该 SKU 的计划格口列表（判断"实际落格号是否计划外"）
        QMap<QString, QStringList> planGridsByWave;
        for (const ReturnWaveItemRecord& it : items)
            planGridsByWave[it.orderCode] << gridKeyOf(it.gridNum);

        // 行数据（先组装再渲染，便于展开/去重/配色）
        struct SkuRow
        {
            QString wave, epc, actualGrid, planGrid, gridTypeText;
            QString planQty, sortedQty, volu, box, time, status;
            bool    mismatch = false;   // 实际落格号不在计划格口内
            bool    pending  = false;   // 计划存在但尚无落格 EPC
        };
        QVector<SkuRow> rows;
        QVector<bool> used(details.size(), false);
        QSet<QString> actualGridSet;    // 实际落格号去重（跨波次同格口算一个）
        QSet<QString> planGridSet;      // 计划格口去重（同上）
        int planRowCount = 0;

        // ① 计划视角：每个计划格口展开其已落格 EPC
        for (const ReturnWaveItemRecord& item : items)
        {
            const QString planKey = gridKeyOf(item.gridNum);
            planGridSet.insert(planKey);

            QString gridTypeText;
            if (item.gridType == "0")      gridTypeText = QString::fromUtf8("分类");
            else if (item.gridType == "1") gridTypeText = QString::fromUtf8("异常");
            else if (item.gridType == "2") gridTypeText = QString::fromUtf8("发货");
            else                           gridTypeText = item.gridType;

            int matched = 0;
            for (int k = 0; k < details.size(); ++k)
            {
                if (used[k]) continue;
                const SortingRecord& d = details[k];
                if (d.orderCode != item.orderCode) continue;
                if (gridKeyOf(d.gridNum) != planKey) continue;   // 归一后比较："007" == "7"

                used[k] = true;
                ++matched;
                actualGridSet.insert(gridKeyOf(d.gridNum));

                SkuRow r;
                r.wave         = d.orderCode;
                r.epc          = d.barcode;                      // EPC编码
                r.actualGrid   = gridKeyOf(d.gridNum);           // 实际落格号
                r.planGrid     = planKey;                        // 计划格口
                r.gridTypeText = gridTypeText;
                r.planQty      = QString::number(item.planQty);
                r.sortedQty    = QString::number(item.sortedQty);
                r.volu         = d.volu.isEmpty() ? item.volu : d.volu;
                r.box          = d.boxcode.isEmpty() ? item.obxCode : d.boxcode;   // 落格容器优先
                r.time         = d.sortTime;
                r.status       = QString::fromUtf8("已分拣");
                rows.append(r);
            }

            if (matched == 0)
            {
                // 计划有这个格口但尚无 EPC 落格 → 保留一行，状态"待分拣"
                SkuRow r;
                r.wave         = item.orderCode;
                r.planGrid     = planKey;
                r.gridTypeText = gridTypeText;
                r.planQty      = QString::number(item.planQty);
                r.sortedQty    = QString::number(item.sortedQty);
                r.volu         = item.volu;
                r.box          = item.obxCode;
                r.status       = QString::fromUtf8("待分拣");
                r.pending      = true;
                rows.append(r);
            }
            ++planRowCount;
        }

        // ② 实绩视角补漏：已落格但不属于任何计划格口的 EPC（实际落格号 ≠ 计划格口）
        for (int k = 0; k < details.size(); ++k)
        {
            if (used[k]) continue;
            const SortingRecord& d = details[k];

            SkuRow r;
            r.wave       = d.orderCode;
            r.epc        = d.barcode;
            r.actualGrid = gridKeyOf(d.gridNum);
            r.planGrid   = planGridsByWave.value(d.orderCode).join("/");
            r.volu       = d.volu;
            r.box        = d.boxcode;
            r.time       = d.sortTime;
            r.status     = QString::fromUtf8("已分拣");
            if (!r.planGrid.isEmpty())
            {
                r.planGrid += QString::fromUtf8("（计划外）");
                r.mismatch  = true;
            }
            rows.append(r);
            actualGridSet.insert(r.actualGrid);
        }

        // ── 渲染：13 列（新增「EPC编码」「实际落格号」）──
        m_tblRecords->setColumnCount(13);
        m_tblRecords->setHorizontalHeaderLabels({
            QString::fromUtf8("序号"),
            QString::fromUtf8("波次号"),
            QString::fromUtf8("SKU编码"),
            QString::fromUtf8("EPC编码"),
            QString::fromUtf8("实际落格号"),
            QString::fromUtf8("计划格口"),
            QString::fromUtf8("格口类型"),
            QString::fromUtf8("计划数量"),
            QString::fromUtf8("已分拣数量"),
            QString::fromUtf8("库位"),
            QString::fromUtf8("容器号"),
            QString::fromUtf8("分拣时间"),
            QString::fromUtf8("状态")
        });
        m_tblRecords->setRowCount(0);
        m_tblRecords->setRowCount(rows.size());

        int epcRowCount = 0;
        for (int i = 0; i < rows.size(); ++i)
        {
            const SkuRow& r = rows[i];

            auto* c0 = new QTableWidgetItem(QString::number(i + 1));
            c0->setTextAlignment(Qt::AlignCenter);
            m_tblRecords->setItem(i, 0, c0);

            m_tblRecords->setItem(i, 1, new QTableWidgetItem(r.wave));
            m_tblRecords->setItem(i, 2, new QTableWidgetItem(sku));

            if (r.pending)
            {
                auto* epcItem = new QTableWidgetItem(QString::fromUtf8("—"));
                epcItem->setTextAlignment(Qt::AlignCenter);
                epcItem->setForeground(QColor("#999999"));
                m_tblRecords->setItem(i, 3, epcItem);
                m_tblRecords->setItem(i, 4, new QTableWidgetItem(QString()));
            }
            else
            {
                ++epcRowCount;
                m_tblRecords->setItem(i, 3, new QTableWidgetItem(r.epc));

                auto* gridItem = new QTableWidgetItem(r.actualGrid);       // 实际落格号
                gridItem->setTextAlignment(Qt::AlignCenter);
                gridItem->setToolTip(QString::fromUtf8("EPC %1 实际落格：%2（WMS编码 %3）")
                    .arg(r.epc).arg(r.actualGrid).arg(gridToWmsCode(r.actualGrid)));
                if (r.mismatch)
                {
                    // 实际落格号与计划格口不一致 → 橙色加粗，便于人工核查
                    gridItem->setForeground(QColor("#FF8C00"));
                    QFont f = gridItem->font();
                    f.setBold(true);
                    gridItem->setFont(f);
                }
                m_tblRecords->setItem(i, 4, gridItem);
            }

            auto* planItem = new QTableWidgetItem(r.planGrid);
            planItem->setTextAlignment(Qt::AlignCenter);
            m_tblRecords->setItem(i, 5, planItem);
            m_tblRecords->setItem(i, 6, new QTableWidgetItem(r.gridTypeText));

            auto* c7 = new QTableWidgetItem(r.planQty);
            c7->setTextAlignment(Qt::AlignCenter);
            m_tblRecords->setItem(i, 7, c7);

            auto* c8 = new QTableWidgetItem(r.sortedQty);
            c8->setTextAlignment(Qt::AlignCenter);
            m_tblRecords->setItem(i, 8, c8);

            m_tblRecords->setItem(i, 9,  new QTableWidgetItem(r.volu));
            m_tblRecords->setItem(i, 10, new QTableWidgetItem(r.box));
            m_tblRecords->setItem(i, 11, new QTableWidgetItem(r.time));

            auto* statusItem = new QTableWidgetItem(r.status);
            statusItem->setTextAlignment(Qt::AlignCenter);
            if (r.pending)
                statusItem->setForeground(QColor("#999999"));
            else if (r.mismatch)
                statusItem->setForeground(QColor("#FF8C00"));
            else
                statusItem->setForeground(QColor("#228B22"));
            m_tblRecords->setItem(i, 12, statusItem);
        }
        m_tblRecords->resizeRowsToContents();

        // ── 统计标签 ──
        QString label = QString::fromUtf8("SKU编码 [%1]：计划 %2 条明细（%3 个格口）｜EPC落格明细 %4 条，实际落在 %5 个格口")
            .arg(sku).arg(planRowCount).arg(planGridSet.size()).arg(epcRowCount).arg(actualGridSet.size());
        if (details.size() >= SORTING_QUERY_MAX_RESULTS)
            label += QString::fromUtf8("（已达单次查询上限 %1 条，可能截断）").arg(SORTING_QUERY_MAX_RESULTS);
        m_lblRecordCount->setText(label);
        // 同品多格口：高亮显示（沿用原口径）
        m_lblRecordCount->setStyleSheet(planGridSet.size() > 1
            ? "font-size: 12px; color: #FF8C00; font-weight: bold;"
            : "font-size: 12px; color: #555;");

        // 更新数据库统计
        SortingStatistics stats = db->statistics();
        m_lblDbStats->setText(QString::fromUtf8("数据库: 总计 %1 条 | 今日 %2 条 | %3 波次 | %4 格口")
            .arg(stats.totalRecords)
            .arg(stats.todayRecords)
            .arg(stats.totalWaves)
            .arg(stats.totalGrids));

        appendLog(QString::fromUtf8("[查询] SKU编码 [%1]：计划 %2 条明细（%3 个格口），EPC落格明细 %4 条（实际落在 %5 个格口）")
            .arg(sku).arg(planRowCount).arg(planGridSet.size()).arg(epcRowCount).arg(actualGridSet.size()));
        return;
    }

    // ★ 按 EPC 查询（原有逻辑）
    {
        // ★ 切换回 EPC 查询模式的列头（★ 2026-09-09 需求6：加"容器号"列）
        m_tblRecords->setColumnCount(11);
        m_tblRecords->setHorizontalHeaderLabels({
            QString::fromUtf8("序号"),
            QString::fromUtf8("波次号"),
            QString::fromUtf8("EPC编码"),
            QString::fromUtf8("SKU编码"),
            QString::fromUtf8("格口号"),
            QString::fromUtf8("容器号"),
            QString::fromUtf8("小车号(首车/尾车)"),
            QString::fromUtf8("件数"),
            QString::fromUtf8("库位"),
            QString::fromUtf8("分拣时间"),
            QString::fromUtf8("状态")
        });
        m_lblRecordCount->setStyleSheet("font-size: 12px; color: #555;");

    QString barcode = m_editQueryBarcode->text().trimmed();
    QDateTime from(m_editQueryDateFrom->date(), QTime(0, 0, 0));
    QDateTime to(m_editQueryDateTo->date(), QTime(23, 59, 59, 999));

    QVector<SortingRecord> records;

    if (!barcode.isEmpty())
    {
        // 按EPC编码查询（已分拣 + 待分拣，用 NOT EXISTS 去重）
        records = db->queryByBarcode(barcode, SORTING_QUERY_MAX_RESULTS);
    }
    else
    {
        // 留空查全部：已分拣 + 待分拣（queryAllWithPending 自动用 NOT EXISTS 去重）
        records = db->queryAllWithPending(SORTING_QUERY_MAX_RESULTS);
    }

    // ★ 2026-09-09 需求3：批量取异常原因（epc → "type: reason"），状态列对异常件显示原因
    QHash<QString, QString> excReasons = db->queryExceptionReasons();

    // 填充表格
    m_tblRecords->setRowCount(0);
    m_tblRecords->setRowCount(records.size());

    for (int i = 0; i < records.size(); ++i)
    {
        const SortingRecord& rec = records[i];

        auto* item0 = new QTableWidgetItem(QString::number(i + 1));
        item0->setTextAlignment(Qt::AlignCenter);
        m_tblRecords->setItem(i, 0, item0);

        m_tblRecords->setItem(i, 1, new QTableWidgetItem(rec.orderCode));
        m_tblRecords->setItem(i, 2, new QTableWidgetItem(rec.barcode));
        m_tblRecords->setItem(i, 3, new QTableWidgetItem(rec.sku));           // ★ SKU编码
        m_tblRecords->setItem(i, 4, new QTableWidgetItem(rec.gridNum));

        // ★ 小车号显示：参考 WCSApp 格式，5字段时显示"首车:xxx ; 尾车: xxx"
        QString carDisplay;
        if (!rec.firstCar.isEmpty() || !rec.lastCar.isEmpty())
        {
            carDisplay = QString::fromUtf8("首车:%1 ; 尾车: %2")
                .arg(rec.firstCar.isEmpty() ? rec.carNum : rec.firstCar)
                .arg(rec.lastCar.isEmpty() ? "--" : rec.lastCar);
        }
        else
        {
            carDisplay = rec.carNum;
        }
        m_tblRecords->setItem(i, 5, new QTableWidgetItem(rec.boxcode));   // ★ 2026-09-09 需求6：容器号
        m_tblRecords->setItem(i, 6, new QTableWidgetItem(carDisplay));

        auto* item5 = new QTableWidgetItem(QString::number(rec.gridCount));
        item5->setTextAlignment(Qt::AlignCenter);
        m_tblRecords->setItem(i, 7, item5);

        m_tblRecords->setItem(i, 8, new QTableWidgetItem(rec.volu));
        m_tblRecords->setItem(i, 9, new QTableWidgetItem(rec.sortTime));

        // 状态列：已分拣=绿色，待分拣=橙色；★ 2026-09-09 需求3：异常件显示原因（红色）
        auto* statusItem = new QTableWidgetItem(rec.status);
        statusItem->setTextAlignment(Qt::AlignCenter);
        if (excReasons.contains(rec.barcode))
        {
            statusItem->setText(QString::fromUtf8("异常: %1").arg(excReasons.value(rec.barcode)));
            statusItem->setForeground(QColor("#D32F2F"));   // 异常红色
            statusItem->setToolTip(excReasons.value(rec.barcode));
        }
        else if (rec.status == QString::fromUtf8("已分拣")) {
            statusItem->setForeground(QColor("#228B22"));  // 森林绿
        } else if (rec.status == QString::fromUtf8("待分拣")) {
            statusItem->setForeground(QColor("#FF8C00"));  // 暗橙色
        }
        m_tblRecords->setItem(i, 10, statusItem);
    }

    // 更新统计标签
    if (!barcode.isEmpty())
    {
        m_lblRecordCount->setText(QString("共 %1 条记录（EPC编码: %2）")
            .arg(records.size()).arg(barcode));
    }
    else
    {
        m_lblRecordCount->setText(QString("共 %1 条记录（%2 ~ %3）")
            .arg(records.size())
            .arg(from.toString("yyyy-MM-dd"))
            .arg(to.toString("yyyy-MM-dd")));
    }

    // 更新数据库统计
    SortingStatistics stats = db->statistics();
    m_lblDbStats->setText(QString("数据库: 总计 %1 条 | 今日 %2 条 | %3 波次 | %4 格口")
        .arg(stats.totalRecords)
        .arg(stats.todayRecords)
        .arg(stats.totalWaves)
        .arg(stats.totalGrids));

    appendLog(QString("[查询] 返回 %1 条记录").arg(records.size()));
    }
}

// ★ 开始分拣按钮点击：手动触发分拣中状态
void MainWindow::onStartSortingClicked()
{
    if (!m_bRunning)
    {
        appendLog("[分拣] 未在接收任务（或正在停止），无法开始分拣", true);
        return;
    }
    if (!m_pServer || !m_pServer->waveManager())
        return;

    WaveManager* wm = m_pServer->waveManager();
    if (wm->status() == WAVE_SORTING)
    {
        appendLog("[分拣] 当前波次为'分拣中'状态，正在分拣", true);
        //m_btnStartSorting->setEnabled(false);
        return;
    }
    else if (wm->status() != WAVE_BOUND)
    {
        appendLog("[分拣] 当前波次非'已绑定'状态，无法开始分拣", true);
        return;
    }

    if (wm->startSorting())
    {
        appendLog(QString("[分拣] 手动开始分拣 orderCode=%1").arg(wm->orderCode()));
        // 按钮由定时器自动刷新为灰色禁用状态（状态已变为 SORTING）
    }
    else
    {
        appendLog("[分拣] 开始分拣失败，请检查波次状态", true);
    }
}

