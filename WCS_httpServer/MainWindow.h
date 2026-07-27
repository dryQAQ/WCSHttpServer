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
#include <QAtomicInteger>
#include <QMutex>
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
    void flushLogBuffer();    // 定时批量刷新日志到UI（防高频卡死）

private:
    void setupUI();           // 构建所有UI控件
    void setupConnections();  // 连接信号槽
    void applyConfig();       // 从ConfigManager读取配置填充UI
    void appendLog(const QString& msg, bool isError = false);  // 追加日志到窗口
    //void updateStatusBar();   // 更新顶部状态条
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

    // ──── TCP 连接状态 UI ────
    QLabel*      m_lblTcpStatus    = nullptr;  // TCP连接状态
    QLabel*      m_lblTcpIp        = nullptr;  // TCP客户端IP
    QLabel*      m_lblTcpUptime    = nullptr;  // 运行时长
    QLabel*      m_lblTcpSend      = nullptr;  // TCP发送统计
    QLabel*      m_lblTcpSendErr   = nullptr;  // TCP发送失败统计
    QLabel*      m_lblTcpRecv      = nullptr;  // TCP接收统计
    QLabel*      m_lblTcpConnCount = nullptr;  // TCP客户端连接数

    // ──── S7 连接状态 UI ────
    QLabel*      m_lblS7Status     = nullptr;  // S7连接状态
    QLabel*      m_lblS7Ip         = nullptr;  // S7 PLC IP
    QLabel*      m_lblS7Send       = nullptr;  // S7发送统计
    QLabel*      m_lblS7SendErr    = nullptr;  // S7发送失败统计
    QLabel*      m_lblS7LockGrids  = nullptr;  // S7锁格数量

    // ──── 最近数据 UI ────
    QLabel*      m_lblLastSendCode = nullptr;  // 最近发送条码
    QLabel*      m_lblLastSendGrid = nullptr;  // 最近发送格口
    QLabel*      m_lblLastSendTime = nullptr;  // 最近发送时间
    QLabel*      m_lblLastRecvCode = nullptr;  // 最近接收条码
    QLabel*      m_lblLastRecvGrid = nullptr;  // 最近接收格口
    QLabel*      m_lblLastRecvTime = nullptr;  // 最近接收时间

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
    QPushButton* m_btnSave         = nullptr;  // 保存配置按钮

    // ──── 日志区 ────
    QTextEdit*   m_txtLog = nullptr;           // 运行日志文本框

    // ──── 日志缓冲（防高频卡死） ────
    QStringList  m_logBuffer;                  // 日志消息缓冲队列
    QMutex       m_logMutex;                   // 缓冲队列互斥锁（备而不用，当前appendLog在主线程）
    QTimer*      m_logFlushTimer = nullptr;    // 日志刷新定时器（100ms，批量刷新）

    // ──── 定时器 ────
    QTimer*      m_timerRefresh = nullptr;     // UI刷新定时器（每秒）

    // ──── 状态 ────
    bool         m_bRunning = false;           // 服务运行状态
    int          m_reportRetryCount = 0;       // 手动回传重试计数（预留，当前未使用）
    QAtomicInteger<qint64> m_plcFeedbackCount{0}; // ★ PLC反馈计数（无锁，高并发安全）
};
