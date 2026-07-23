#include "MainWindow.h"
#include "ConfigManager.h"
#include "log_center.h"
#include "hlog1.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QApplication>
#include <QDateTime>
#include <QCloseEvent>
#include <QMessageBox>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("WMS退货HTTP服务 V1.0");
    resize(960, 720);
    setMinimumSize(800, 600);

    setupUI();
    ConfigManager::instance()->load();
    applyConfig();
    setupConnections();

    appendLog("程序已启动，等待操作...");

    // 自动启动
    onStartStop();
}

MainWindow::~MainWindow()
{
    if (m_pServer) { m_pServer->stop(); }
    if (m_timerRefresh) m_timerRefresh->stop();
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

    m_lblServerStatus = new QLabel("● 已停止");
    m_lblServerStatus->setStyleSheet("font-size: 14px; color: #f44336;");
    m_lblPort = new QLabel("端口: 8191");

    serverLayout->addWidget(m_btnStartStop);
    serverLayout->addWidget(m_lblServerStatus);
    serverLayout->addStretch();
    serverLayout->addWidget(m_lblPort);

    // ═══════════════════════════════════════════
    // 第二行：波次信息面板
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

    m_spinWmsPort = new QSpinBox();     m_spinWmsPort->setRange(1, 65535); m_spinWmsPort->setValue(8191);
    m_editFeedbackUrl = new QLineEdit();
    m_editAppkey      = new QLineEdit();
    m_chkTestEnv      = new QCheckBox("测试环境");
    m_spinTimeout     = new QSpinBox(); m_spinTimeout->setRange(0, 1440); m_spinTimeout->setSuffix(" 分钟");

    QPushButton* btnSave = new QPushButton("保存配置");
    btnSave->setMinimumHeight(32);

    int cr = 0;
    cfgLayout->addWidget(new QLabel("端口:"), cr, 0); cfgLayout->addWidget(m_spinWmsPort, cr++, 1);
    cfgLayout->addWidget(new QLabel("回传URL:"),  cr, 0); cfgLayout->addWidget(m_editFeedbackUrl, cr++, 1);
    cfgLayout->addWidget(new QLabel("AppKey:"),    cr, 0); cfgLayout->addWidget(m_editAppkey, cr++, 1);
    cfgLayout->addWidget(new QLabel("波次超时:"),  cr, 0); cfgLayout->addWidget(m_spinTimeout, cr++, 1);
    cfgLayout->addWidget(m_chkTestEnv, cr, 0);             cfgLayout->addWidget(btnSave, cr++, 1);

    // ═══════════════════════════════════════════
    // 日志区
    // ═══════════════════════════════════════════
    QGroupBox* grpLog = new QGroupBox("运行日志");
    QVBoxLayout* logLayout = new QVBoxLayout(grpLog);

    m_txtLog = new QTextEdit();
    m_txtLog->setReadOnly(true);
    m_txtLog->document()->setMaximumBlockCount(5000);
    m_txtLog->setStyleSheet("font-family: Consolas, 'Microsoft YaHei'; font-size: 12px;");

    QPushButton* btnClearLog = new QPushButton("清空日志");

    logLayout->addWidget(m_txtLog);
    logLayout->addWidget(btnClearLog);
    connect(btnClearLog, &QPushButton::clicked, this, &MainWindow::onClearLog);

    // ═══════════════════════════════════════════
    // 组装布局
    // ═══════════════════════════════════════════
    mainLayout->addWidget(grpServer);
    mainLayout->addWidget(grpWave);
    mainLayout->addWidget(grpConfig);
    mainLayout->addWidget(grpLog, 1); // 日志区占剩余空间
}

void MainWindow::setupConnections()
{
    connect(m_btnStartStop, &QPushButton::clicked, this, &MainWindow::onStartStop);
    connect(m_btnManualReport, &QPushButton::clicked, this, &MainWindow::onManualReport);

    // 定时刷新（每秒）
    m_timerRefresh = new QTimer(this);
    connect(m_timerRefresh, &QTimer::timeout, this, &MainWindow::onRefreshTimer);
    m_timerRefresh->start(1000);
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
    if (m_bRunning)
    {
        if (m_pServer) m_pServer->stop();
        m_pServer = nullptr;
        m_pClient = nullptr;
        m_bRunning = false;

        m_btnStartStop->setText("启动服务");
        m_btnStartStop->setStyleSheet(
            "QPushButton { background-color: #4CAF50; color: white; font-size: 14px; font-weight: bold; "
            "border-radius: 4px; padding: 6px 16px; }"
            "QPushButton:hover { background-color: #45a049; }");
        m_lblServerStatus->setText("● 已停止");
        m_lblServerStatus->setStyleSheet("font-size: 14px; color: #f44336;");
        appendLog("服务已手动停止");
    }
    else
    {
        m_pServer = new HttpServer(this);
        m_pClient = new HttpClient(this);

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
            appendLog(QString("HTTP服务已启动 端口=%1").arg(port));

            connect(m_pServer, &HttpServer::waveReadyToReport, this, [this](const QString& orderCode) {
                if (!m_pClient) return;
                int sumLocation = m_pServer->waveManager()->sumLocation();
                appendLog(QString("自动回传波次 %1 sumLocation=%2").arg(orderCode).arg(sumLocation));
                m_pClient->sendWaveComplete(orderCode, sumLocation);
            });
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
    cfg.wmsListenPort  = m_spinWmsPort->value();
    cfg.waveTimeoutMin = m_spinTimeout->value();
    cfg.useTestEnv     = m_chkTestEnv->isChecked();

    if (cfg.useTestEnv) {
        cfg.feedbackTestUrl = m_editFeedbackUrl->text();
        cfg.appkeyTest      = m_editAppkey->text();
    } else {
        cfg.feedbackUrl     = m_editFeedbackUrl->text();
        cfg.appkey          = m_editAppkey->text();
    }

    if (ConfigManager::instance()->save())
        appendLog("配置已保存");
    else
        appendLog("配置保存失败！", true);
}

void MainWindow::onClearLog()
{
    m_txtLog->clear();
}

void MainWindow::appendLog(const QString& msg, bool isError)
{
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    QString color = isError ? "#f44336" : "#212121";
    QString text = QString("<span style='color:#888;'>[%1]</span> "
                           "<span style='color:%2;'>%3</span>")
                       .arg(timestamp).arg(color).arg(msg.toHtmlEscaped());

    m_txtLog->append(text);

    // 同时写入log_center
    if (isError)
        LogCenter::Instance()->wcs_run_log_warn(false, msg);
    else
        LogCenter::Instance()->wcs_run_log_warn(true, msg);
}

void MainWindow::onRefreshWave()
{
    updateWavePanel();
}
