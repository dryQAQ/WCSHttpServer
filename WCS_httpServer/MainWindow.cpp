#include "MainWindow.h"
#include "ConfigManager.h"
#include "log_center.h"
#include "hlog1.h"
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
#include <QVector>
#include <QTextCursor>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("WMS退货HTTP服务 -- 默鑫 V1.0");
    resize(960, 820);
    setMinimumSize(800, 700);

    setupUI();
    ConfigManager::instance()->load();
    applyConfig();
    setupConnections();

    appendLog("程序已启动，等待操作...");

    // ★ 创建日志刷新定时器（100ms，防高频场景下 QTextEdit 卡死）
    m_logFlushTimer = new QTimer(this);
    m_logFlushTimer->setInterval(100);
    connect(m_logFlushTimer, &QTimer::timeout, this, &MainWindow::flushLogBuffer);
    m_logFlushTimer->start();

    // 自动启动
    onStartStop();
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
    mainLayout->setSpacing(8);

    // ═══════════════════════════════════════════
    // 第一行：服务控制区
    // ═══════════════════════════════════════════
    QGroupBox* grpServer = new QGroupBox("服务控制");
    QHBoxLayout* serverLayout = new QHBoxLayout(grpServer);

    m_btnStartStop = new QPushButton("启动服务");
    m_btnStartStop->setMinimumWidth(120);
    m_btnStartStop->setMinimumHeight(36);
    m_btnStartStop->setStyleSheet(
        "QPushButton { background-color: #4CAF50; color: white; font-size: 14px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #45a049; }");

    m_lblServerStatus = new QLabel(QCoreApplication::translate("MainWindow", "● 已停止"));
    m_lblServerStatus->setStyleSheet("font-size: 14px; color: #f44336;");

    // ★ 端口标签直接从配置读取（避免硬编码，确保重启后显示正确）
    ConfigManager* cfgMgr = ConfigManager::instance();
    cfgMgr->load();
    m_lblPort = new QLabel(QString("端口: %1").arg(cfgMgr->config().wmsListenPort));

    serverLayout->addWidget(m_btnStartStop);
    serverLayout->addWidget(m_lblServerStatus);
    serverLayout->addStretch();
    serverLayout->addWidget(m_lblPort);

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

    m_btnManualReport = new QPushButton("手动回传");
    m_btnManualReport->setMinimumHeight(32);

    int row = 0;
    waveLayout->addWidget(makeLabel("波次号:"),    row, 0); waveLayout->addWidget(m_lblWaveCode,    row++, 1);
    waveLayout->addWidget(makeLabel("状态:"),      row, 0); waveLayout->addWidget(m_lblWaveStatus,  row++, 1);
    waveLayout->addWidget(makeLabel("SKU数:"),     row, 0); waveLayout->addWidget(m_lblSkuCount,    row++, 1);
    waveLayout->addWidget(makeLabel("已分拣:"),    row, 0); waveLayout->addWidget(m_lblSorted,      row++, 1);
    waveLayout->addWidget(makeLabel("异常:"),      row, 0); waveLayout->addWidget(m_lblException,   row++, 1);
    waveLayout->addWidget(makeLabel("格口数:"),    row, 0); waveLayout->addWidget(m_lblSumLocation, row++, 1);
    waveLayout->addWidget(makeLabel("耗时:"),      row, 0); waveLayout->addWidget(m_lblElapsed,     row++, 1);
    waveLayout->addWidget(makeLabel("上波次:"),    row, 0); waveLayout->addWidget(m_lblLastWave,    row++, 1);
    waveLayout->addWidget(m_btnManualReport,       row, 0, 1, 2);

    // ═══════════════════════════════════════════
    // 第三行：配置区（紧凑）
    // ═══════════════════════════════════════════
    QGroupBox* grpConfig = new QGroupBox("配置");
    QGridLayout* cfgLayout = new QGridLayout(grpConfig);

    m_spinWmsPort     = new QSpinBox();     m_spinWmsPort->setRange(1, 65535);
    m_editFeedbackUrl = new QLineEdit();
    m_editAppkey      = new QLineEdit();
    m_chkTestEnv      = new QCheckBox("测试环境");
    m_spinTimeout     = new QSpinBox(); m_spinTimeout->setRange(0, 1440); m_spinTimeout->setSuffix(" 分钟");
    m_btnSave         = new QPushButton("保存配置");

    m_btnSave->setMinimumHeight(32);

    int cr = 0;
    cfgLayout->addWidget(new QLabel("端口:"), cr, 0);     cfgLayout->addWidget(m_spinWmsPort, cr++, 1);
    cfgLayout->addWidget(new QLabel("回传URL:"),  cr, 0); cfgLayout->addWidget(m_editFeedbackUrl, cr++, 1);
    cfgLayout->addWidget(new QLabel("AppKey:"),    cr, 0); cfgLayout->addWidget(m_editAppkey, cr++, 1);
    cfgLayout->addWidget(new QLabel("波次超时:"),  cr, 0); cfgLayout->addWidget(m_spinTimeout, cr++, 1);
    cfgLayout->addWidget(m_chkTestEnv, cr, 0);             cfgLayout->addWidget(m_btnSave, cr++, 1);

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
    mainLayout->addWidget(grpServer);
    mainLayout->addWidget(grpPlc);
    mainLayout->addWidget(grpWave);
    mainLayout->addWidget(grpConfig);
    mainLayout->addWidget(grpLog, 1); // 日志区占剩余空间
}

void MainWindow::setupConnections()
{
    connect(m_btnStartStop, &QPushButton::clicked, this, &MainWindow::onStartStop);
    connect(m_btnManualReport, &QPushButton::clicked, this, &MainWindow::onManualReport);
    connect(m_btnSave, &QPushButton::clicked, this, &MainWindow::onSaveConfig);

    // 定时刷新（每秒）
    m_timerRefresh = new QTimer(this);
    connect(m_timerRefresh, &QTimer::timeout, this, &MainWindow::onRefreshTimer);
    m_timerRefresh->start(1000);

    // ★ 初始化时加载配置到 UI 控件
    applyConfig();
}

void MainWindow::applyConfig()
{
    AppConfig& cfg = ConfigManager::instance()->config();
    m_spinWmsPort->setValue(cfg.wmsListenPort);
    m_editFeedbackUrl->setText(cfg.activeFeedbackUrl());
    m_editAppkey->setText(cfg.activeAppkey());
    m_chkTestEnv->setChecked(cfg.useTestEnv);
    m_spinTimeout->setValue(cfg.waveTimeoutMin);

    m_lblPort->setText(QString("端口: %1").arg(cfg.wmsListenPort));
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

        m_btnStartStop->setText(QCoreApplication::translate("MainWindow", "启动服务"));
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
        appendLog("服务已手动停止");
    }
    else    //状态：关闭 --> 开启
    {
        m_pServer = new HttpServer(this);
        m_pClient = new HttpClient(this);
        m_pPlcMgr = m_pServer->plcManager();  // ★ 获取PLC管理器引用

        AppConfig& cfg = ConfigManager::instance()->config();
        m_pClient->setUrl(cfg.activeFeedbackUrl());
        m_pClient->setAppkey(cfg.activeAppkey());

        m_pServer->waveManager()->setWaveTimeoutMin(cfg.waveTimeoutMin);
        m_pServer->waveManager()->setMaxRetry(cfg.maxRetryCount);

        int port = m_spinWmsPort->value();

        if (m_pServer->start(port))
        {
            m_bRunning = true;
            m_btnStartStop->setText("停止服务");
            m_btnStartStop->setStyleSheet(
                "QPushButton { background-color: #f44336; color: white; font-size: 14px; font-weight: bold; "
                "border-radius: 4px; padding: 6px 16px; }"
                "QPushButton:hover { background-color: #d32f2f; }");
            m_lblServerStatus->setText("● 运行中");
            m_lblServerStatus->setStyleSheet("font-size: 14px; color: #4CAF50;");

            m_lblPort->setText(QString("端口: %1").arg(port));

            appendLog(QString("HTTP服务已启动 端口=%1").arg(port));

            connect(m_pServer, &HttpServer::waveReadyToReport, this, [this](const QString& orderCode) {
                if (!m_pClient) return;
                int sumLocation = m_pServer->waveManager()->sumLocation();
                appendLog(QString("自动回传波次 %1 sumLocation=%2").arg(orderCode).arg(sumLocation));
                m_pClient->sendWaveComplete(orderCode, sumLocation);
            });

            // ★ 回传结果处理：成功→已完成，失败→异常（避免状态卡在"回传中"）
            connect(m_pClient, &HttpClient::reportResult, this,
                [this](const QString& orderCode, bool success, const QString& body) {
                    Q_UNUSED(body);
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

            // ★ 连接HttpServer日志信号到UI日志区
            connect(m_pServer, &HttpServer::logMessage, this, &MainWindow::appendLog);

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

                // ★ 业务信号：每个反馈都需处理（HttpServer 用于 markSorted）
                //    使用 QueuedConnection 确保在主线程执行
                connect(m_pPlcMgr, &PlcManager::plcFeedbackReceived, this,
                    [this](const QString& code, const QString& grid, const QString& car) {
                        Q_UNUSED(code); Q_UNUSED(grid); Q_UNUSED(car);
                        // 业务逻辑由 HttpServer 处理，此处仅更新统计计数
                        m_plcFeedbackCount.fetchAndAddRelaxed(1);
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
                            int showCount = qMin(entries.size(), 3);
                            for (int i = 0; i < showCount; ++i)
                            {
                                const auto& e = entries[i];
                                if (i > 0) summary += "\n";
                                summary += QString("  code=%1 → grid=%2 car=%3")
                                    .arg(e.code).arg(e.grid).arg(e.car);
                            }
                            if (entries.size() > 3)
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
    if (s.running && s.clientCount > 0)
    {
        m_lblTcpStatus->setText(QString("● TCP: 已连接"));
        m_lblTcpStatus->setStyleSheet("font-size: 13px; color: #4CAF50; font-weight: bold;");
        m_lblTcpIp->setText(QString("%1:%2").arg(s.lastIp.isEmpty() ? "?" : s.lastIp).arg(s.lastPort));
        m_lblTcpIp->setStyleSheet("font-size: 13px; color: #2196F3;");
    }
    else if (s.running)
    {
        m_lblTcpStatus->setText(QString("● TCP: 监听中"));
        m_lblTcpStatus->setStyleSheet("font-size: 13px; color: #FF9800; font-weight: bold;");
        m_lblTcpIp->setText(QString(":%1").arg(s.port));
        m_lblTcpIp->setStyleSheet("font-size: 13px; color: #888;");
    }
    else
    {
        m_lblTcpStatus->setText(QCoreApplication::translate("MainWindow", "TCP: 未启动"));
        m_lblTcpStatus->setStyleSheet("font-size: 13px; color: #f44336; font-weight: bold;");
        m_lblTcpIp->setText("");
        m_lblTcpIp->setStyleSheet("font-size: 13px; color: #888;");
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

void MainWindow::onManualReport()
{
    if (!m_pServer || !m_pClient) return;

    WaveSnapshot snap = m_pServer->waveManager()->snapshot();
    if (snap.orderCode.isEmpty())
    {
        QMessageBox::information(this, "提示", "没有活跃的波次可回传");
        return;
    }

    auto ret = QMessageBox::question(this, "手动回传",
        QString("确定回传波次 %1 吗？\n格口数: %2  已分拣: %3/%4")
            .arg(snap.orderCode).arg(snap.sumLocation)
            .arg(snap.sortedCount).arg(snap.totalRecv));

    if (ret == QMessageBox::Yes)
    {
        m_pServer->waveManager()->setState(WAVE_COMPLETING);
        m_pClient->sendWaveComplete(snap.orderCode, snap.sumLocation);
        appendLog(QString("手动回传波次 %1 sumLocation=%2").arg(snap.orderCode).arg(snap.sumLocation));
    }
}

void MainWindow::onSaveConfig()
{
    AppConfig& cfg = ConfigManager::instance()->config();
    int oldPort = cfg.wmsListenPort;
    int newPort = m_spinWmsPort->value();

    cfg.wmsListenPort  = newPort;
    cfg.waveTimeoutMin = m_spinTimeout->value();
    cfg.useTestEnv     = m_chkTestEnv->isChecked();

    if (cfg.useTestEnv) {
        cfg.feedbackTestUrl = m_editFeedbackUrl->text();
        cfg.appkeyTest      = m_editAppkey->text();
    } else {
        cfg.feedbackUrl     = m_editFeedbackUrl->text();
        cfg.appkey          = m_editAppkey->text();
    }

    if (!ConfigManager::instance()->save())
    {
        appendLog("配置保存失败！", true);
        return;
    }

    // ★ 更新 UI 显示，确保配置值正确反映在界面上
    applyConfig();

    // ★ 实时生效：更新运行中组件的配置
    if (m_bRunning)
    {
        // 更新回传URL和AppKey
        if (m_pClient)
        {
            m_pClient->setUrl(cfg.activeFeedbackUrl());
            m_pClient->setAppkey(cfg.activeAppkey());
        }

        // 更新波次超时
        if (m_pServer && m_pServer->waveManager())
        {
            m_pServer->waveManager()->setWaveTimeoutMin(cfg.waveTimeoutMin);
        }

        // 端口变更需要重启服务
        if (newPort != oldPort)
        {
            appendLog(QString("端口已变更 %1→%2，需重启服务生效").arg(oldPort).arg(newPort), true);
            m_lblPort->setText(QString("端口: %1 (需重启)").arg(newPort));
        }
        else
        {
            appendLog(QString("配置已保存并生效 url=%1 timeout=%2min")
                .arg(cfg.activeFeedbackUrl()).arg(cfg.waveTimeoutMin));
        }
    }
    else
    {
        appendLog("配置已保存");
    }
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
// flushLogBuffer — 定时批量刷新日志缓冲到 UI
//
// 设计要点：
//   - 每 100ms 由定时器触发一次，将缓冲队列中的日志批量写入 QTextEdit
//   - 单次最多刷新 100 条，超出部分留到下次刷新（防 UI 长时间阻塞）
//   - 100ms 间隔平衡了实时性和性能：高频场景下 UI 更新频率被限制在 10次/秒
//   - 日志文件写入（log_center）不受缓冲影响，已在 appendLog 中实时完成
// ============================================================================
void MainWindow::flushLogBuffer()
{
    // 批量取出缓冲队列
    QStringList batch;
    {
        QMutexLocker locker(&m_logMutex);
        if (m_logBuffer.isEmpty()) return;
        batch.swap(m_logBuffer);  // O(1) 交换，清空缓冲区
    }

    // 截断保护：单次最多刷新 100 条，超出的重新放回队首
    constexpr int MAX_BATCH = 100;
    if (batch.size() > MAX_BATCH)
    {
        // 保留前 MAX_BATCH 条，其余重新入队（下次刷新）
        QStringList remaining = batch.mid(MAX_BATCH);
        {
            QMutexLocker locker(&m_logMutex);
            // 剩余部分插入到队首，保持顺序
            for (int i = remaining.size() - 1; i >= 0; --i)
                m_logBuffer.prepend(remaining[i]);
        }
        batch = batch.mid(0, MAX_BATCH);
    }

    // 批量写入 UI（关掉自动格式化，加速 append）
    m_txtLog->setUpdatesEnabled(false);
    for (const QString& line : batch)
        m_txtLog->append(line);
    m_txtLog->setUpdatesEnabled(true);

    // 滚动到底部
    m_txtLog->moveCursor(QTextCursor::End);
}

void MainWindow::onRefreshWave()
{
    updateWavePanel();
}
