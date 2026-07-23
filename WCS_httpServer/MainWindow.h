#pragma once
// ============================================================================
// MainWindow.h — WMS退货HTTP服务主窗口
// 包含：服务状态指示 | 波次信息面板 | 实时日志 | 配置管理
// ============================================================================

#include <QMainWindow>
#include <QTimer>
#include <QLabel>
#include <QTableWidget>
#include <QTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QLineEdit>
#include <QCheckBox>
#include "HttpServer.h"
#include "HttpClient.h"
#include "ConfigManager.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onStartStop();
    void onRefreshWave();
    void onManualReport();
    void onSaveConfig();
    void onClearLog();

    // 定时刷新
    void onRefreshTimer();

private:
    void setupUI();
    void setupConnections();
    void applyConfig();
    void appendLog(const QString& msg, bool isError = false);
    void updateStatusBar();
    void updateWavePanel();

    // ──── 核心组件 ────
    HttpServer*  m_pServer  = nullptr;
    HttpClient*  m_pClient  = nullptr;

    // ──── UI控件 ────
    // 服务控制区
    QPushButton* m_btnStartStop   = nullptr;
    QLabel*      m_lblServerStatus = nullptr;
    QLabel*      m_lblPort         = nullptr;

    // 波次面板
    QLabel*      m_lblWaveCode    = nullptr;
    QLabel*      m_lblWaveStatus  = nullptr;
    QLabel*      m_lblSkuCount    = nullptr;
    QLabel*      m_lblSorted      = nullptr;
    QLabel*      m_lblException   = nullptr;
    QLabel*      m_lblSumLocation = nullptr;
    QLabel*      m_lblElapsed     = nullptr;
    QLabel*      m_lblLastWave    = nullptr;
    QPushButton* m_btnManualReport = nullptr;

    // 配置区
    QSpinBox*    m_spinWmsPort     = nullptr;
    QLineEdit*   m_editFeedbackUrl = nullptr;
    QLineEdit*   m_editAppkey      = nullptr;
    QCheckBox*   m_chkTestEnv      = nullptr;
    QSpinBox*    m_spinTimeout     = nullptr;

    // 日志区
    QTextEdit*   m_txtLog = nullptr;

    // 定时器
    QTimer*      m_timerRefresh = nullptr;

    // 状态
    bool         m_bRunning = false;
    int          m_reportRetryCount = 0;
};
