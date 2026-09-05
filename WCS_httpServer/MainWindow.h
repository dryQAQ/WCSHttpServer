#pragma once
// ============================================================================
// MainWindow.h — WMS退货HTTP服务主窗口
//
// 界面布局：
//   ┌─ 服务控制区 ─┐ ┌─ 波次信息面板 ────────────────────────────────────┐
//   │ 启动/停止     │ │ 波次号 / 状态 / SKU数 / 已分拣 / 异常 / 格口数   │
//   │ 端口 / PLC状态│ │ 耗时 / 上波次                                      │
//   ├─ PLC综合状态 ─┤ └──────────────────────────────────────────────────┘
//   │ TCP/S7连接统计 │
//   ├─ 容器绑定状态 ┤
//   │ 66格口→容器   │
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
#include <QGridLayout>
#include <QTextEdit>
#include <QPushButton>
#include <QAtomicInteger>
#include <QMutex>
#include <QLineEdit>
#include <QDateEdit>
#include <QTableWidget>
#include <QSpinBox>
#include <QComboBox>
#include "HttpServer.h"
#include "HttpClient.h"
#include "PlcManager.h"
#include "ConfigManager.h"
#include "SortingDatabase.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;  // 窗口关闭时结束任务

private slots:
    void onStartStop();       // 启动/结束任务按钮
    void onRefreshBindings(); // 刷新容器绑定状态
    void onClearLog();        // 清空日志窗口
    void onRefreshTimer();    // 每秒定时刷新UI
    void flushLogBuffer();    // 定时批量刷新日志到UI（防高频卡死）
    void onQueryRecords();    // ★ 查询分拣记录
    void onStartSortingClicked(); // ★ 开始分拣按钮点击
    void doActualStop();     // ★ 实际执行服务停止（H8回传完成后调用）

private:
    // ★ 2026-09-02 修复"结束任务卡死/闪退"：停止流程阶段状态机
    //   StopNone   — 未在停止流程中（服务运行或已停止）
    //   StopEnding — 已点击"结束任务"，触发 H8 回传，正在等待结果（可再次点击取消等待）
    //   StopDone   — 停止收尾已完成（幂等出口，防重复执行）
    enum StopPhase { StopNone = 0, StopEnding, StopDone };
    StopPhase m_stopPhase = StopNone;

    void setupUI();           // 构建所有UI控件
    void setupConnections();  // 连接信号槽
    void applyConfig();       // 从ConfigManager读取配置填充UI
    void appendLog(const QString& msg, bool isError = false);  // 追加日志到窗口
    void updateWavePanel();   // 更新波次信息面板
    void updatePlcPanel();    // 刷新PLC状态面板（连接数/收发统计/运行时间）
    void updateBindingPanel();// 刷新容器绑定面板（66格口×容器号）
    // ──── 核心组件 ────
    HttpServer*  m_pServer  = nullptr;   // HTTP Server（内部持有 PlcManager/WaveManager/TaskQueue 等）
    HttpClient*  m_pClient  = nullptr;   // WMS回传客户端
    PlcManager*  m_pPlcMgr  = nullptr;   // PLC管理器引用（生命周期由 HttpServer 管理，此处仅持有句柄）
    SortingDatabase* m_pQueryDb = nullptr;  // ★ UI 查询数据库（单例引用）

    // ──── 服务控制区 UI ────
    QPushButton* m_btnStartStop    = nullptr;  // 启动/停止按钮
    QLabel*      m_lblServerStatus = nullptr;  // 服务状态指示（●运行中/○已停止）
    QLabel*      m_lblPort         = nullptr;  // 监听端口显示
    QSpinBox*    m_spinBindCount   = nullptr;  // 期望绑定数量（波次下发时校验全部绑定用，默认66）
    QLabel*      m_lblBindCountHint = nullptr;  // 期望绑定数量提示标签

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
    QLabel*      m_lblLastSendCode = nullptr;  // 最近发送EPC编码
    QLabel*      m_lblLastSendGrid = nullptr;  // 最近发送格口
    QLabel*      m_lblLastSendTime = nullptr;  // 最近发送时间
    QLabel*      m_lblLastRecvCode = nullptr;  // 最近接收EPC编码
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
    QPushButton* m_btnStartSorting = nullptr;  // ★ 开始分拣按钮（手动触发分拣中状态）

    // ──── 容器绑定面板 UI（92格口 6列×16行）────
    QWidget*     m_bindingWidget   = nullptr;  // 绑定状态容器
    QGridLayout* m_bindingGrid     = nullptr;  // 网格布局
    QLabel*      m_bindingLabels[BINDING_SLOT_COUNT] = {};    // 状态指示圆点指针
    QLabel*      m_bindingBoxLabels[BINDING_SLOT_COUNT] = {}; // ★ 容器号标签指针（避免findChildren）
    int          m_bindingCols     = 4;        // 每行列数
    int          m_bindingRows     = BINDING_SLOT_COUNT / m_bindingCols;
    QLabel*      m_lblBoundCount   = nullptr;  // 已绑定数量
    QLabel*      m_lblUnboundCount = nullptr;  // 未绑定数量
    bool         m_bindingDirty    = false;     // ★ 绑定数据变更标记（避免无效刷新）

    // ──── 日志区 ────
    QTextEdit*   m_txtLog = nullptr;           // 运行日志文本框

    // ──── 日志缓冲（防高频卡死） ────
    QStringList  m_logBuffer;                  // 日志消息缓冲队列
    QMutex       m_logMutex;                   // 缓冲队列互斥锁
    QTimer*      m_logFlushTimer = nullptr;    // 日志刷新定时器（100ms，高负载自动降频）
    int          m_logFlushIntervalMs = LOG_FLUSH_INTERVAL_MS; // ★ 动态调整的刷新间隔
    int          m_logDropCount       = 0;     // ★ 丢弃的日志计数（高负载时）

    // ──── 定时器 ────
    QTimer*      m_timerRefresh = nullptr;     // UI刷新定时器（每秒）

    // ──── 状态 ────
    bool         m_bRunning = false;           // 服务运行状态
    int          m_reportRetryCount = 0;       // 手动回传重试计数（预留，当前未使用）
    QAtomicInteger<qint64> m_plcFeedbackCount{0}; // ★ PLC反馈计数（无锁，高并发安全）
    bool         m_lastTcpConnected = false;   // ★ 缓存TCP状态（避免冗余setStyleSheet）
    bool         m_lastS7Connected  = false;   // ★ 缓存S7状态

    // ──── 分拣记录查询 UI ────
    QComboBox*   m_cmbQueryMode     = nullptr;   // ★ 查询模式：按EPC查询 / 按SKU查询格口分配
    QLineEdit*   m_editQueryBarcode  = nullptr;   // EPC编码查询输入
    QLineEdit*   m_editQuerySku      = nullptr;   // ★ SKU编码查询输入
    QDateEdit*   m_editQueryDateFrom = nullptr;   // 查询起始日期
    QDateEdit*   m_editQueryDateTo   = nullptr;   // 查询结束日期
    QPushButton* m_btnQueryRecords   = nullptr;   // 查询按钮
    QPushButton* m_btnQueryClear     = nullptr;   // 清空结果
    QTableWidget* m_tblRecords       = nullptr;   // 查询结果表格
    QLabel*      m_lblRecordCount    = nullptr;   // 记录统计标签
    QLabel*      m_lblDbStats        = nullptr;   // 数据库统计标签
    QTimer*      m_dbCleanupTimer    = nullptr;   // ★ 数据库清理定时器（每日凌晨）
    QTimer*      m_stopTimeoutTimer = nullptr;   // ★ 停止超时安全网（30秒）
};
