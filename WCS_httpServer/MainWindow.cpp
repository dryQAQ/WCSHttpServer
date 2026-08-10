#include "MainWindow.h"
#include "ConfigManager.h"
#include "log_center.h"
#include "hlog1.h"
#include "SortingDatabase.h"
#include "define.h"
#include "LifecycleLogger.h"
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
#include <QJsonArray>
#include <QJsonObject>
#include <QTextCursor>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("WCS退货HTTP服务 -- V1.0");
    resize(960, 1100);
    setMinimumSize(860, 950);

    // ★ UI 查询数据库独立打开，不受服务启停影响
    {
        QString dbPath = QCoreApplication::applicationDirPath() + "/" + SORTING_DB_FILE;
        if (m_queryDb.open(dbPath))
        {
            // 静默成功，不刷日志
        }
    }

    setupUI();
    ConfigManager::instance()->load();
    applyConfig();
    setupConnections();

    appendLog("程序已启动，等待操作...");

    // ★ 创建日志刷新定时器（100ms，防高频场景下 QTextEdit 卡死）
    m_logFlushTimer = new QTimer(this);
    m_logFlushTimer->setInterval(LOG_FLUSH_INTERVAL_MS);
    connect(m_logFlushTimer, &QTimer::timeout, this, &MainWindow::flushLogBuffer);
    m_logFlushTimer->start();

    //自动启动：
    // onStartStop();
    // 不自动启动，等待用户点击"开始启动"
}

MainWindow::~MainWindow()
{
    if (m_pServer) { m_pServer->stop(); }
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
    if (m_bRunning)
    {
        auto ret = QMessageBox::question(this, "确认退出",
            "HTTP服务正在运行中，确定退出吗？",
            QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes)
        {
            event->ignore();
            return;
        }
    }
    onStartStop();
    event->accept();
}

void MainWindow::setupUI()
{
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout* mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(2);

    // ═══════════════════════════════════════════
    // 第一行：服务控制区
    // ═══════════════════════════════════════════
    QGroupBox* grpServer = new QGroupBox("服务控制");
    QVBoxLayout* serverLayout = new QVBoxLayout(grpServer);
    serverLayout->setAlignment(Qt::AlignCenter);

    m_btnStartStop = new QPushButton("开始启动");
    m_btnStartStop->setMinimumWidth(120);
    m_btnStartStop->setMinimumHeight(36);
    m_btnStartStop->setStyleSheet(
        "QPushButton { background-color: #4CAF50; color: white; font-size: 14px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #45a049; }");

    m_lblServerStatus = new QLabel(QCoreApplication::translate("MainWindow", "● 已停止"));
    m_lblServerStatus->setStyleSheet("font-size: 14px; color: #f44336;");
    m_lblServerStatus->setAlignment(Qt::AlignCenter);

    // ★ 端口标签直接从配置读取（避免硬编码，确保重启后显示正确）
    ConfigManager* cfgMgr = ConfigManager::instance();
    cfgMgr->load();
    m_lblPort = new QLabel(QString("端口: %1").arg(cfgMgr->config().wmsListenPort));
    m_lblPort->setAlignment(Qt::AlignCenter);

    // ★ 期望绑定数量输入（默认66，每批次可配置不同数量）
    QHBoxLayout* bindCountRow = new QHBoxLayout();
    m_lblBindCountHint = new QLabel(QCoreApplication::translate("MainWindow", "期望绑定数量:"));
    m_lblBindCountHint->setStyleSheet("font-size: 13px;");
    m_spinBindCount = new QSpinBox();
    m_spinBindCount->setMinimum(1);
    m_spinBindCount->setMaximum(999);
    m_spinBindCount->setValue(cfgMgr->config().expectedBindCount);
    m_spinBindCount->setToolTip(QCoreApplication::translate("MainWindow", "波次下发时校验绑定数量，默认66。每批次可修改"));
    m_spinBindCount->setStyleSheet("QSpinBox { font-size: 13px; padding: 2px; }");
    m_spinBindCount->setFixedWidth(80);
    bindCountRow->addStretch();
    bindCountRow->addWidget(m_lblBindCountHint);
    bindCountRow->addWidget(m_spinBindCount);
    bindCountRow->addStretch();

    serverLayout->addWidget(m_btnStartStop);
    serverLayout->addWidget(m_lblServerStatus);
    serverLayout->addWidget(m_lblPort);
    serverLayout->addLayout(bindCountRow);

    // ═══════════════════════════════════════════
    // 第二行：PLC 综合状态面板
    // ═══════════════════════════════════════════
    QGroupBox* grpPlc = new QGroupBox(QCoreApplication::translate("MainWindow", "PLC 综合状态"));
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
    plcOuterLayout->addWidget(line1);
    plcOuterLayout->addLayout(lastDataRow1);
    plcOuterLayout->addLayout(lastDataRow2);

    // ═══════════════════════════════════════════
    // 第二行：波次信息面板
    // ═══════════════════════════════════════════
    QGroupBox* grpWave = new QGroupBox("波次信息");
    QGridLayout* waveLayout = new QGridLayout(grpWave);
    //grpWave->setFixedHeight(9 * 30);

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
    m_lblElapsed     = makeValue();
    m_lblLastWave    = makeValue();

    int row = 0;
    waveLayout->addWidget(makeLabel("波次号:"),    row, 0); waveLayout->addWidget(m_lblWaveCode,    row++, 1);
    waveLayout->addWidget(makeLabel("状态:"),      row, 0); waveLayout->addWidget(m_lblWaveStatus,  row++, 1);
    waveLayout->addWidget(makeLabel("SKU数:"),     row, 0); waveLayout->addWidget(m_lblSkuCount,    row++, 1);
    waveLayout->addWidget(makeLabel("已分拣:"),    row, 0); waveLayout->addWidget(m_lblSorted,      row++, 1);
    waveLayout->addWidget(makeLabel("异常:"),      row, 0); waveLayout->addWidget(m_lblException,   row++, 1);
    waveLayout->addWidget(makeLabel("格口数:"),    row, 0); waveLayout->addWidget(m_lblSumLocation, row++, 1);
    waveLayout->addWidget(makeLabel("耗时:"),      row, 0); waveLayout->addWidget(m_lblElapsed,     row++, 1);
    waveLayout->addWidget(makeLabel("上波次:"),    row, 0); waveLayout->addWidget(m_lblLastWave,    row++, 1);

    // ═══════════════════════════════════════════
    // 容器绑定状态面板（66格口，6列×11行可拓展网格）
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
    QLabel* bindHint = new QLabel("绿色=已绑定  灰色=未绑定");
    bindHint->setStyleSheet("font-size: 12px; color: #888;");
    bindBtnRow->addWidget(btnRefreshBind);
    bindBtnRow->addWidget(bindHint);
    bindBtnRow->addStretch();

    // 已绑定/未绑定计数
    m_lblBoundCount = new QLabel();
    m_lblUnboundCount = new QLabel();
    m_lblBoundCount->setStyleSheet("font-size: 13px; font-weight: bold; color: #4CAF50; padding: 0 8px;");
    m_lblUnboundCount->setStyleSheet("font-size: 13px; font-weight: bold; color: #E53935; padding: 0 8px;");
    bindBtnRow->addWidget(m_lblBoundCount);
    bindBtnRow->addWidget(m_lblUnboundCount);

    connect(btnRefreshBind, &QPushButton::clicked, this, &MainWindow::onRefreshBindings);

    // 可滚动区域——容纳所有格口绑定指示器
    QScrollArea* scrollBinding = new QScrollArea();
    scrollBinding->setWidgetResizable(true);
    scrollBinding->setMinimumHeight(180);
    scrollBinding->setMaximumHeight(180);
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
    // ★ 分拣记录查询面板
    // ═══════════════════════════════════════════
    QGroupBox* grpQuery = new QGroupBox(QCoreApplication::translate("MainWindow", "分拣记录查询"));
    QVBoxLayout* queryLayout = new QVBoxLayout(grpQuery);

    // ── 查询条件行 ──
    QHBoxLayout* queryCondRow = new QHBoxLayout();
    queryCondRow->addWidget(new QLabel(QCoreApplication::translate("MainWindow", "条码:")));
    m_editQueryBarcode = new QLineEdit();
    m_editQueryBarcode->setPlaceholderText(QCoreApplication::translate("MainWindow", "输入条码查询（留空查全部）"));
    m_editQueryBarcode->setMinimumWidth(180);
    queryCondRow->addWidget(m_editQueryBarcode);

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
    m_tblRecords->setColumnCount(9);
    m_tblRecords->setHorizontalHeaderLabels({
        QCoreApplication::translate("MainWindow", "序号"),
        QCoreApplication::translate("MainWindow", "波次号"),
        QCoreApplication::translate("MainWindow", "条码"),
        QCoreApplication::translate("MainWindow", "格口号"),
        QCoreApplication::translate("MainWindow", "小车号"),
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
    // 回车触发查询
    connect(m_editQueryBarcode, &QLineEdit::returnPressed, this, &MainWindow::onQueryRecords);

    // ═══════════════════════════════════════════
    // 日志区
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
    // 组装布局
    // ═══════════════════════════════════════════
    // 第一行：服务控制 + PLC 综合状态（水平）
    QHBoxLayout* rowTop = new QHBoxLayout();
    grpServer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    rowTop->addWidget(grpServer);
    rowTop->addWidget(grpPlc, 1);
    mainLayout->addLayout(rowTop);

    // 第二行：波次信息 + 运行日志（水平）
    QHBoxLayout* rowMid = new QHBoxLayout();
    grpWave->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    rowMid->addWidget(grpWave);
    rowMid->addWidget(grpLog, 1);
    mainLayout->addLayout(rowMid, 1); // 占剩余垂直空间

    mainLayout->addWidget(grpBinding);
    mainLayout->addWidget(grpQuery);  // ★ 分拣记录查询面板
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
}

void MainWindow::applyConfig()
{
    AppConfig& cfg = ConfigManager::instance()->config();
    m_lblPort->setText(QString("端口: %1").arg(cfg.wmsListenPort));
    if (m_spinBindCount)
        m_spinBindCount->setValue(cfg.expectedBindCount);
}

// ============================================================================
// 槽函数
// ============================================================================

void MainWindow::onStartStop()
{
    if (m_bRunning)//状态：开启 --> 关闭
    {
        if (m_pServer) m_pServer->stop();
        m_pServer = nullptr;
        m_pClient = nullptr;
        m_pPlcMgr = nullptr;
        m_bRunning = false; //状态切换

        m_btnStartStop->setText(QCoreApplication::translate("MainWindow", "开始启动"));
        m_btnStartStop->setStyleSheet(
            "QPushButton { background-color: #4CAF50; color: white; font-size: 14px; font-weight: bold; "
            "border-radius: 4px; padding: 6px 16px; }"
            "QPushButton:hover { background-color: #45a049; }");
        m_lblServerStatus->setText(QCoreApplication::translate("MainWindow", "● 已停止"));
        m_lblServerStatus->setStyleSheet("font-size: 14px; color: #f44336;");
        // 重置TCP状态
        m_lblTcpStatus->setText(QCoreApplication::translate("MainWindow", "TCP: 未连接"));
        m_lblTcpStatus->setStyleSheet("font-size: 13px; color: #888; font-weight: bold;");
        m_lblTcpIp->setText("");
        m_lblTcpSend->setText("TCP发送: 0");
        m_lblTcpSendErr->setText("TCP失败: 0");
        m_lblTcpRecv->setText("TCP接收: 0");
        m_lblTcpConnCount->setText("客户端: 0");
        m_lblTcpUptime->setText("");
        // 重置S7状态
        m_lblS7Status->setText(QCoreApplication::translate("MainWindow", "S7: 未连接"));
        m_lblS7Status->setStyleSheet("font-size: 13px; color: #888; font-weight: bold;");
        m_lblS7Ip->setText("");
        m_lblS7Send->setText("S7发送: 0");
        m_lblS7SendErr->setText("S7失败: 0");
        m_lblS7LockGrids->setText("锁格: 0");
        // 重置最近数据
        m_lblLastSendCode->setText(QCoreApplication::translate("MainWindow", "最近发送: --"));
        m_lblLastSendGrid->setText("");
        m_lblLastSendTime->setText("");
        m_lblLastRecvCode->setText(QCoreApplication::translate("MainWindow", "最近接收: --"));
        m_lblLastRecvGrid->setText("");
        m_lblLastRecvTime->setText("");
        m_lastTcpConnected = false;  // ★ 重置缓存状态
        m_lastS7Connected  = false;

        // ★ 停止时清除容器绑定（内存 + XML + UI）
        {
            AppConfig& c = ConfigManager::instance()->config();
            c.containerBindings.clear();
            ConfigManager::instance()->saveNow();  // ★ 立即写盘，不等延迟
        }
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
        if (m_lblUnboundCount) m_lblUnboundCount->setText("未绑定: 66");
        m_bindingDirty = false;

        appendLog("服务已手动停止");
    }
    else    //状态：关闭 --> 开启
    {
        // ★ 服务始终允许启动以接收 WMS 的绑定请求和波次数据
        //    容器绑定校验移至波次推送入口（InsertWaveInfo），避免循环依赖：
        //    服务必须运行才能接收 BindingLatticePort 请求完成绑定
        m_pServer = new HttpServer(this);

        AppConfig& cfg = ConfigManager::instance()->config();

        // ★ 从配置文件恢复容器绑定
        m_pServer->loadContainerBindings(cfg.containerBindings);
        m_bindingDirty = true;  // ★ 初始加载后标记为脏，首次刷新时更新面板

        // ★ 设置期望绑定数量（从配置文件加载，默认66）
        m_pServer->setExpectedBindCount(cfg.expectedBindCount);
        m_pClient = new HttpClient(this);
        m_pPlcMgr = m_pServer->plcManager();  // ★ 获取PLC管理器引用
        m_pClient->setUrl(cfg.activeFeedbackUrl());
        m_pClient->setAppkey(cfg.activeAppkey());
        m_pClient->setTimeout(cfg.httpTimeoutMs);
        m_pClient->setRfidQueryUrl(cfg.rfidQueryUrl);  // ★ RFID SKU-EPC 绑定查询 URL
        m_pServer->setHttpClient(m_pClient);              // ★ 设置 HttpClient 供 RFID 查询使用

        // ★ 从配置文件加载 API 路由路径
        m_pServer->setApiInsertWaveInfo(cfg.apiInsertWaveInfo);
        m_pServer->setApiBindingLatticePort(cfg.apiBindingLatticePort);
        m_pServer->setApiInsertWaveIn(cfg.apiInsertWaveIn);
        m_pServer->setApiRfidCarNumReport(cfg.apiRfidCarNumReport);  // ★ RFID 小车号推送

        m_pServer->waveManager()->setWaveTimeoutMin(cfg.waveTimeoutMin);
        m_pServer->waveManager()->setMaxRetry(cfg.maxRetryCount);

        int port = cfg.wmsListenPort;

        if (m_pServer->start(port))
        {
            m_bRunning = true;
            m_btnStartStop->setText("结束任务");
            m_btnStartStop->setStyleSheet(
                "QPushButton { background-color: #f44336; color: white; font-size: 14px; font-weight: bold; "
                "border-radius: 4px; padding: 6px 16px; }"
                "QPushButton:hover { background-color: #d32f2f; }");
            m_lblServerStatus->setText("● 运行中");
            m_lblServerStatus->setStyleSheet("font-size: 14px; color: #4CAF50;");

            m_lblPort->setText(QString("端口: %1 %2")
                .arg(port)
                .arg(cfg.useTestEnv ? "(测试)" : "(正式)"));
            m_lblPort->setStyleSheet(cfg.useTestEnv
                ? "font-size: 14px; color: #FF9800; font-weight: bold;"
                : "font-size: 14px; color: #f44336; font-weight: bold;");

            appendLog(QString("HTTP服务已启动 端口=%1 环境=%2")
                .arg(port)
                .arg(cfg.useTestEnv ? "测试" : "正式"));

            // ★ 配置摘要日志
            {
                QString summary;
                summary += "\n══════════════════ 配置摘要 ══════════════════\n";
                summary += QString(" 监听端口:        %1 (WMS) / %2 (PLC)\n")
                    .arg(port).arg(cfg.plcListenPort);
                summary += QString(" 回传URL:         %1 (%2)\n")
                    .arg(cfg.activeFeedbackUrl())
                    .arg(cfg.useTestEnv ? "测试" : "正式");
                summary += QString(" AppKey:          %1\n").arg(cfg.activeAppkey());
                summary += QString(" 仓库:            %1\n").arg(cfg.warehouseCode);
                summary += QString(" 货主:            %1\n").arg(cfg.goodsOwner);
                summary += QString(" 波次超时:        %1分钟(%2), 期望绑定: %3\n")
                    .arg(cfg.waveTimeoutMin)
                    .arg(cfg.waveTimeoutMin == 0 ? "不超时" : QString::number(cfg.waveTimeoutMin) + "分钟")
                    .arg(cfg.expectedBindCount);
                summary += QString(" 重试:            %1次, 间隔: %2秒\n")
                    .arg(OUTBOX_RETRY_MAX_DEFAULT).arg(OUTBOX_RETRY_INTERVAL_SEC);
                summary += QString(" 日志:            保留%1天\n").arg(cfg.logRetainDays);
                summary += QString(" 配置文件版本:    %1 (软件版本: %2)\n")
                    .arg(cfg.configVersion).arg(CONFIG_VERSION);
                if (cfg.configVersion != CONFIG_VERSION)
                {
                    summary += QString(" ⚠ 配置文件版本不匹配! 请检查配置\n");
                }
                summary += "══════════════════════════════════════════════";
                appendLog(summary);
                WCS_LOG_INFO("配置摘要: 端口=%d/%d URL=%s env=%s warehouse=%s goodsOwner=%s waveTimeout=%d bindCount=%d",
                    port, cfg.plcListenPort, cfg.activeFeedbackUrl().toLocal8Bit().data(),
                    cfg.useTestEnv ? "test" : "prod",
                    cfg.warehouseCode.toLocal8Bit().data(), cfg.goodsOwner.toLocal8Bit().data(),
                    cfg.waveTimeoutMin, cfg.expectedBindCount);
            }

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

                    // ★ S5: 锁格回传（context 以 "lockGrid_" 开头）
                    if (orderCode.startsWith("lockGrid_"))
                    {
                        appendLog(QString("[锁格] 回传结果 grid=%1 success=%2")
                            .arg(orderCode.mid(9)).arg(success));
                        return;
                    }

                    // ★ S6: 完结回传 Outbox（H8，context 以 "end_" 开头）
                    if (orderCode.startsWith("end_"))
                    {
                        QString msgId = orderCode.mid(4); // 去掉 "end_" 前缀
                        if (m_pServer)
                        {
                            m_pServer->onEndReplyFinished(msgId, success, body);
                        }
                        appendLog(QString("[完结回传] 回传结果 msgId=%1 success=%2")
                            .arg(msgId).arg(success));
                        return;
                    }

                    // ★ 完结回传（H8 波次完结通知WMS，原有逻辑）
                    WaveManager* wm = m_pServer ? m_pServer->waveManager() : nullptr;
                    if (!wm) return;
                    if (success)
                    {
                        appendLog(QString("回传成功 orderCode=%1").arg(orderCode));
                        wm->setState(WAVE_CLEANED);
                    }
                    else
                    {
                        appendLog(QString("回传失败 orderCode=%1（仍可手动重试）").arg(orderCode), true);
                        wm->setState(WAVE_ERROR);
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
                    appendLog(QString("[满箱回传] 发送回传 msgId=%1 order=%2").arg(msgId).arg(orderCode));
                    m_pClient->sendGenericFeedback(payload, "fullbox_" + msgId);
                });

            // ★ S6 完结回传 → WMS（H8 波次完结通知，T-S6-03）
            // endReportReady 携带 msgId，HttpClient 返回后路由到 onEndReplyFinished
            connect(m_pServer, &HttpServer::endReportReady, this,
                [this](const QJsonObject& payload, const QString& msgId) {
                    if (!m_pClient) return;
                    QString orderCode = payload["head"].toObject()["orderCode"].toString();
                    appendLog(QString("[完结回传] 发送回传 msgId=%1 order=%2").arg(msgId).arg(orderCode));
                    m_pClient->sendGenericFeedback(payload, "end_" + msgId);
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
                connect(m_pPlcMgr, &PlcManager::plcFeedbackBatch, this,
                    [this](const QVector<PlcFeedbackEntry>& entries) {
                        if (entries.isEmpty()) return;
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
                    appendLog(QString("[S7] 锁格 grid=%1").arg(grid), true);
                }, Qt::QueuedConnection);
                connect(m_pPlcMgr, &PlcManager::gridUnlocked, this, [this](const QString& grid) {
                    updatePlcPanel();
                    appendLog(QString("[S7] 解锁 grid=%1").arg(grid));
                }, Qt::QueuedConnection);

                // ★ 启动S7连接（与 WCSApp 一致，服务启动后自动连接S7 PLC）
                m_pPlcMgr->connectS7();
            }

            

            // ★ 启动后清理过期数据库记录
            if (m_pServer->sortingDb())
                m_pServer->sortingDb()->cleanupOldRecords(SORTING_DB_RETAIN_DAYS);
        }
        else
        {
            appendLog("服务启动失败！", true);
            m_pServer = nullptr;
            m_pClient = nullptr;
        }
    }
}

void MainWindow::onRefreshTimer()
{
    if (m_bRunning && m_pServer && m_pServer->waveManager())
    {
        updateWavePanel();
        updatePlcPanel();
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

void MainWindow::updateWavePanel()
{
    WaveSnapshot snap = m_pServer->waveManager()->snapshot();

    m_lblWaveCode->setText(snap.orderCode.isEmpty() ? "-- 等待波次 --" : snap.orderCode);
    m_lblWaveStatus->setText(snap.statusText);
    m_lblSkuCount->setText(QString::number(snap.skuCount));
    m_lblSorted->setText(QString("%1 / %2").arg(snap.sortedCount).arg(snap.totalRecv));
    m_lblException->setText(QString::number(snap.exceptionCount));
    m_lblSumLocation->setText(QString::number(snap.sumLocation));

    if (snap.elapsedSec >= 0)
    {
        int min = snap.elapsedSec / 60;
        int sec = snap.elapsedSec % 60;
        m_lblElapsed->setText(QString("%1分%2秒").arg(min).arg(sec));
    }

    // 颜色提示
    if (snap.waveStatus == WAVE_SORTING)
        m_lblWaveStatus->setStyleSheet("font-size: 13px; font-weight: bold; color: #FF9800;");
    else if (snap.waveStatus >= WAVE_COMPLETING)
        m_lblWaveStatus->setStyleSheet("font-size: 13px; font-weight: bold; color: #4CAF50;");
    else
        m_lblWaveStatus->setStyleSheet("font-size: 13px; font-weight: bold; color: #2196F3;");
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

    // S7 收发统计
    m_lblS7Send->setText(QString("S7发送: %1").arg(s.s7SendCount));
    m_lblS7SendErr->setText(QString("S7失败: %1").arg(s.s7SendErrCount));
    if (s.s7SendErrCount > 0)
        m_lblS7SendErr->setStyleSheet("font-size: 13px; color: #f44336; font-weight: bold;");
    else
        m_lblS7SendErr->setStyleSheet("font-size: 13px; color: #888;");

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



// ============================================================================
// 容器绑定状态
// ============================================================================

void MainWindow::onRefreshBindings()
{
    updateBindingPanel();
    appendLog("容器绑定状态已刷新");
}

void MainWindow::updateBindingPanel()
{
    if (!m_pServer) return;

    QMap<QString, QString> bindings = m_pServer->getContainerBindings();
    int boundCount = 0;

    for (int i = 0; i < BINDING_SLOT_COUNT; ++i)
    {
        int gridNum = i + 1;
        QString gridKey = QString("%1").arg(gridNum, GRID_KEY_PADDING, 10, QChar('0'));
        QString boxCode = bindings.value(gridKey, "");

        QLabel* lblStatus = m_bindingLabels[i];
        QLabel* lblBox    = m_bindingBoxLabels[i];
        if (!lblStatus || !lblBox) continue;

        if (!boxCode.isEmpty())
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
    m_lblUnboundCount->setText(QString("未绑定: %1").arg(unboundCount));
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
// ★ onQueryRecords — 分拣记录查询
// ============================================================================

void MainWindow::onQueryRecords()
{
    SortingDatabase* db = &m_queryDb;
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

    QString barcode = m_editQueryBarcode->text().trimmed();
    QDateTime from(m_editQueryDateFrom->date(), QTime(0, 0, 0));
    QDateTime to(m_editQueryDateTo->date(), QTime(23, 59, 59, 999));

    QVector<SortingRecord> records;

    if (!barcode.isEmpty())
    {
        // 按条码查询（已分拣 + 待分拣，用 NOT EXISTS 去重）
        records = db->queryByBarcode(barcode, SORTING_QUERY_MAX_RESULTS);
    }
    else
    {
        // 留空查全部：已分拣 + 待分拣（queryAllWithPending 自动用 NOT EXISTS 去重）
        records = db->queryAllWithPending(SORTING_QUERY_MAX_RESULTS);
    }

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
        m_tblRecords->setItem(i, 3, new QTableWidgetItem(rec.gridNum));
        m_tblRecords->setItem(i, 4, new QTableWidgetItem(rec.carNum));

        auto* item5 = new QTableWidgetItem(QString::number(rec.gridCount));
        item5->setTextAlignment(Qt::AlignCenter);
        m_tblRecords->setItem(i, 5, item5);

        m_tblRecords->setItem(i, 6, new QTableWidgetItem(rec.volu));
        m_tblRecords->setItem(i, 7, new QTableWidgetItem(rec.sortTime));

        // 状态列：已分拣=绿色，待分拣=橙色
        auto* statusItem = new QTableWidgetItem(rec.status);
        statusItem->setTextAlignment(Qt::AlignCenter);
        if (rec.status == QString::fromUtf8("已分拣")) {
            statusItem->setForeground(QColor("#228B22"));  // 森林绿
        } else if (rec.status == QString::fromUtf8("待分拣")) {
            statusItem->setForeground(QColor("#FF8C00"));  // 暗橙色
        }
        m_tblRecords->setItem(i, 8, statusItem);
    }

    // 更新统计标签
    if (!barcode.isEmpty())
    {
        m_lblRecordCount->setText(QString("共 %1 条记录（条码: %2）")
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

