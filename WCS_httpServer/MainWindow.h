#pragma once
// ============================================================================
// MainWindow.h — WMS退货HTTP服务主窗口
//
// 界面布局：
//   ┌─ 服务控制区 ─┐ ┌─ 波次信息面板 ────────────────────────────────────┐
//   │ 启动/停止     │ │ 波次号 / 状态 / SKU数 / 已分拣 / 异常 / 格口数   │
//   │ 端口 / PLC状态│ │ 耗时 / 上波次 / 手动回传                          │
//   ├─ PLC状态 ────┤ └──────────────────────────────────────────────────┘
//   │ 连接数/收发统计│
//   ├─ 配置区 ─────┤
//   │ 端口/URL/AppKey│
//   ├─ 运行日志 ───┤
//   │ QTextEdit     │
//   └──────────────┘
//
// 定时刷新：QTimer 每秒查询 HttpServer 状态并更新 UI
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
#include "PlcManager.h"
#include "ConfigManager.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;  // 窗口关闭时停止服务

private slots:
    void onStartStop();       // 启动/停止服务按钮
    void onRefreshWave();     // 手动刷新波次状态
    void onManualReport();    // 手动触发波次回传
    void onSaveConfig();      // 保存配置到XML
    void onClearLog();        // 清空日志窗口
    void onRefreshTimer();    // 每秒定时刷新UI

private:
    void setupUI();           // 构建所有UI控件
    void setupConnections();  // 连接信号槽
    void applyConfig();       // 从ConfigManager读取配置填充UI
    void appendLog(const QString& msg, bool isError = false);  // 追加日志到窗口
    void updateStatusBar();   // 更新顶部状态条
    void updateWavePanel();   // 更新波次信息面板
    void updatePlcPanel();    // 刷新PLC状态面板（连接数/收发统计/运行时间）

    // ──── 核心组件 ────
    HttpServer*  m_pServer  = nullptr;   // HTTP Server（内部持有 PlcManager/WaveManager/TaskQueue 等）
    HttpClient*  m_pClient  = nullptr;   // WMS回传客户端
    PlcManager*  m_pPlcMgr  = nullptr;   // PLC管理器引用（生命周期由 HttpServer 管理，此处仅持有句柄）

    // ──── 服务控制区 UI ────
    QPushButton* m_btnStartStop    = nullptr;  // 启动/停止按钮
    QLabel*      m_lblServerStatus = nullptr;  // 服务状态指示（●运行中/○已停止）
    QLabel*      m_lblPort         = nullptr;  // 监听端口显示
    QLabel*      m_lblPlcStatus    = nullptr;  // PLC连接状态文字

    // ──── PLC状态面板 UI ────
    QLabel*      m_lblPlcConnCount = nullptr;  // PLC连接客户端数量
    QLabel*      m_lblPlcSendRecv  = nullptr;  // 发送/接收计数
    QLabel*      m_lblPlcUptime    = nullptr;  // PLC服务运行时长

    // ──── 波次面板 UI ────
    QLabel*      m_lblWaveCode     = nullptr;  // 当前波次号
    QLabel*      m_lblWaveStatus   = nullptr;  // 波次状态（空闲/已接收/分拣中/回传中/已完成）
    QLabel*      m_lblSkuCount     = nullptr;  // SKU种类数
    QLabel*      m_lblSorted       = nullptr;  // 已分拣数量
    QLabel*      m_lblException    = nullptr;  // 异常数量
    QLabel*      m_lblSumLocation  = nullptr;  // 去重格口总数
    QLabel*      m_lblElapsed      = nullptr;  // 波次耗时
    QLabel*      m_lblLastWave     = nullptr;  // 上一个波次号
    QPushButton* m_btnManualReport = nullptr;  // 手动回传按钮（异常恢复用）

    // ──── 配置区 UI ────
    QSpinBox*    m_spinWmsPort     = nullptr;  // WMS监听端口编辑框
    QLineEdit*   m_editFeedbackUrl = nullptr;  // 回传URL编辑框
    QLineEdit*   m_editAppkey      = nullptr;  // AppKey编辑框
    QCheckBox*   m_chkTestEnv      = nullptr;  // 测试环境复选框
    QSpinBox*    m_spinTimeout     = nullptr;  // 波次超时设置（分钟）

    // ──── 日志区 ────
    QTextEdit*   m_txtLog = nullptr;           // 运行日志文本框

    // ──── 定时器 ────
    QTimer*      m_timerRefresh = nullptr;     // UI刷新定时器（每秒）

    // ──── 状态 ────
    bool         m_bRunning = false;           // 服务运行状态
    int          m_reportRetryCount = 0;       // 手动回传重试计数（预留，当前未使用）
};
