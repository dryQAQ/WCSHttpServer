#pragma once
// ============================================================================
// MainWindow.h — WMS退货HTTP服务主窗口
//
// 界面布局（★ 2026-09-13 UI改版：第二行改为"左侧标签页"多页窗口）
//   第一行（保持不变）：任务接收控制 ｜ 设备状态(PLC/RFID) ｜ 波次信息
//   第二行：QTabWidget（标签在左侧），6 页——
//     ① 容器绑定状态  ② 分拣记录查询  ③ 波次数据历史记录
//     ④ ★计划分配表  ⑤ 实时面板       ⑥ 运行日志
//     （★ 2026-09-14 新增第④页：该波次"每个产品在各格口的计划/已落/在途/余量"，
//       客户要求放在独立窗口页，**不在波次信息面板里增加内容**）
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
#include <QHash>
#include <QSet>        // ★ 2026-09-16 本次一键生成的 msgId 集合（失败回执归因）
#include <QVector>     // ★ 2026-09-16 绑定面板外框/初始样式缓存
#include <QList>       // ★ 2026-09-15 实时面板原始报文悬停缓存（FIFO 淘汰顺序）
#include "HttpServer.h"
#include "HttpClient.h"
#include "PlcManager.h"
#include "ConfigManager.h"
#include "SortingDatabase.h"

class QDialog;     // ★ 2026-09-07 效率统计弹窗指针（仅在 .cpp 中定义具体类）
class QTabWidget;  // ★ 2026-09-13 第二行多页窗口（标签在左侧）
class QSplitter;   // ★ 第一行水平分隔条（默认宽度分配用，完整类型在 .cpp 中使用）
class QFrame;      // ★ 2026-09-16 容器绑定面板外框（整格底色载体）

// ════════════════════════════════════════════════════════════════════════════
// ★ 2026-09-16 现场需求②⑦：容器绑定面板的「四色底色 + 隐藏异常口」口径
//   状态判定/配色/隐藏规则的**唯一实现**在 tests/BindingPanelPolicy.h 的纯函数里
//   （产品侧与本仓库回归自测 tests/test_binding_panel_policy.cpp 共用同一口径，
//    避免"界面底色/优先级被改坏而无人发现"）。
//   四色（客户指定）：绿=已绑定 / 橙=满箱锁格 / 红=已解锁·待重绑 / 灰=未绑定
//   优先级：锁格 > 待重绑 > 已绑定 > 未绑定
//   ★ 构建注意：该头文件位于 tests/，vcxproj 与 tests/run_tests.bat 的包含目录均已加入
//     $(ProjectDir)..\tests（Windows 上 MSVC 支持 include 路径中的正斜杠）。
// ════════════════════════════════════════════════════════════════════════════
#include "../tests/BindingPanelPolicy.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;  // 窗口关闭时结束任务
    // ★ 2026-09-08 UI调整：默认最大化后按"各行两大部分各占一半"布置一次列宽
    void changeEvent(QEvent* event) override;

private:
    // ★ 2026-09-08 UI调整：水平分隔条默认等分（波次信息占首行一半）
    //   ★ 2026-09-13：第二行改为标签页后，此处只处理第一行的分栏
    void applyDefaultColumnWidths();

private slots:
    void onStartStop();       // 启动/结束任务按钮
    void onRefreshBindings(); // 刷新容器绑定状态
    void onClearAllGridBinds();  // ★ 2026-09-07 清空格口容器绑定（人工重置，DB归档留史）
    void onRefreshWaveRecords(); // ★ 手动刷新「波次数据历史记录」列表
    void onViewWaveQueue();      // ★ 2026-09-08 查看接收波次队列（弹窗：接收新任务 + 剩余待执行波次）
    void onResendSelectedH7();     // ★ 重传满箱切换(H7)（服务控制区；选中行优先，否则当前波次）
    void onResendSelectedH8();     // ★ 重传任务完结(H8)（服务控制区；选中行优先，否则当前波次）
    void onResumeSelectedWave();   // ★ 切换选中波次（恢复其进度继续；★ 2026-09-16 终态波次拒绝切回）
    // ★ 2026-09-17 只读查看选中波次的格口绑定明细（每格取该波次内最后一条 = 切回时恢复的口径）
    void onViewWaveBinds();
    // ★ 2026-09-17 绑定落库失败即时告警（此前只写 data.log，现场表现为"面板绿、库里空"）
    void onBindPersistFailed(const QString& grid, const QString& box, const QString& orderCode);
    void onStartNewWaveTask();     // ★ 新任务：保存当前波次进度与数据，清空等待接收新波次
    // ★ 2026-09-16 需求④：H7 报文最终失败（重试耗尽）→ 本次一键计数归因 + 刷新计数标签
    void onFullboxMessageFailed(const QString& msgId, const QString& orderCode, const QString& grid);
    void onClearLog();        // 清空日志窗口
    void onRefreshTimer();    // 每秒定时刷新UI
    void flushLogBuffer();    // 定时批量刷新日志到UI（防高频卡死）
    void onQueryRecords();    // ★ 查询分拣记录
    void onStartSortingClicked(); // ★ 开始分拣按钮点击
    void doActualStop();     // ★ 实际执行「停止任务接收」收尾（H8回传完成/超时/取消后调用；设备保持连接）
    void onOpenEffChart();   // ★ 2026-09-07 打开 RFID 推送效率统计弹窗（QCustomPlot）
    // ★ 2026-09-13 需求：波次信息「异常」旁按钮 —— 弹窗查看本波次全部异常 EPC 信息
    void onViewExceptions();
    // ★ 2026-09-13 需求：实时面板驻留行的超时打标（"待落格/未落格（无反馈）"）
    void refreshLivePanelPendingRows();
    // ★ 2026-09-13 需求：EPC 全信息窗（异常弹窗与查询结果共用）
    void showEpcDetail(const QString& epc);    // ★ 2026-09-13 需求：按容器号查询渲染（查该容器下全部 EPC 物件明细+计划对照）
    void renderContainerQuery();
    // ★ 2026-09-13 需求：超计划预警明细弹窗（落了几件/哪个格口容器/计划几件/多余几件）
    void showOverplanWarningDialog();
    void refreshOverplanWarning();   // 刷新面板「预警」数字与按钮可用态
    // ★ 2026-09-13 需求：一键满箱回传（对当前所有已绑定容器逐个执行 H7 满箱回传）
    void onOneKeyFullbox();

private:
    // ★ 2026-09-02 修复"结束任务卡死/闪退"：停止流程阶段状态机
    //   StopNone   — 未在停止流程中（接收运行或空闲）
    //   StopEnding — 已点击"结束任务"，触发 H8 回传，正在等待结果（可再次点击取消等待）
    //   StopDone   — 停止接收收尾已完成（幂等出口，防重复执行）
    enum StopPhase { StopNone = 0, StopEnding, StopDone };
    StopPhase m_stopPhase = StopNone;

    void setupUI();           // 构建所有UI控件
    void setupConnections();  // 连接信号槽
    // ★ 2026-09-06 设备连接与任务接收解耦：程序启动时执行一次——
    //   创建常驻 HttpServer/HttpClient + 一次性配置注入/信号连接 + 设备层(PLC/RFID)自动连接
    void setupCore();
    void applyConfig();       // 从ConfigManager读取配置填充UI
    void appendLog(const QString& msg, bool isError = false);  // 追加日志到窗口
    void updateWavePanel();   // 更新波次信息面板
    void updatePlcPanel();    // 刷新PLC状态面板（连接数/收发统计/运行时间）
    void updateRfidStatus();  // ★ 刷新RFID连接状态标签（连接状态变化时记录日志）
    void updateBindingPanel();// 刷新容器绑定面板（66格口×容器号）
    void refreshFailedCombos(); // ★ 2026-09-08 刷新 H7 失败格口 / H8 失败波次两个下拉
    static QString formatTimeFirst(const QString& dbTime); // ★ 2026-09-08 「更新时间」时间在前（HH:mm:ss yyyy-MM-dd）
    QString selectedOrCurrentWaveOrder(); // ★ 重传目标解析：列表选中行优先，否则当前内存波次
    void openConfigEditor();  // ★ 2026-09-07 设置按钮：XML 配置编辑对话框（保存即热生效）
    void applyLiveConfig();   // ★ 2026-09-07 应用可热生效配置项（回传URL/AppKey/method等）
    // ──── 核心组件（★ 2026-09-06 常驻：程序启动建一次，退出才析构）────
    HttpServer*  m_pServer  = nullptr;   // HTTP Server（内部持有 PlcManager/WaveManager/TaskQueue 等）
    HttpClient*  m_pClient  = nullptr;   // WMS回传客户端
    PlcManager*  m_pPlcMgr  = nullptr;   // PLC管理器引用（生命周期由 HttpServer 管理，此处仅持有句柄）
    SortingDatabase* m_pQueryDb = nullptr;  // ★ UI 查询数据库（单例引用）

    // ──── 服务控制区 UI ────
    QPushButton* m_btnStartStop    = nullptr;  // 「开始接收任务/结束任务」按钮
    QLabel*      m_lblServerStatus = nullptr;  // 服务状态指示（●接收中/●停止中/●未接收任务）
    QLabel*      m_lblPort         = nullptr;  // 监听端口显示
    QSpinBox*    m_spinBindCount   = nullptr;  // 期望绑定数量（波次下发时校验全部绑定用，默认1）
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

    // ──── RFID 连接状态 UI（★ 2026-09-06 设备层常驻，程序启动即自动连接）────
    QLabel*      m_lblRfidStatus   = nullptr;  // RFID连接状态
    QLabel*      m_lblRfidIp       = nullptr;  // RFID服务端地址显示
    bool         m_lastRfidConnected = false;  // ★ 缓存RFID状态（避免重复日志/样式）
    bool         m_rfidStatusInited = false;   // ★ 首次刷新标记（先绘图、状态变化后才记日志）
    bool         m_rfidStatusLog    = false;   // ★ 是否已产生过连接日志（首次"连接中"不记断开日志）

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
    QLabel*      m_lblSorted       = nullptr;  // 已分拣件数（PLC 落格反馈累计件次）
    QLabel*      m_lblPlanQty      = nullptr;  // ★ 2026-09-13 计划件数（orderQty，与已分拣/异常对照）
    QLabel*      m_lblException    = nullptr;  // ★ 异常 = 当前仍在异常口的件数（去重 EPC，可被成功落格清理）
    QLabel*      m_lblExcBin       = nullptr;  // ★ 异常口（= 仍在异常口、尚未处理完的件数；成功落格即递减）
    QLabel*      m_lblExcTrace     = nullptr;  // ★ 2026-09-13 异常留痕条数（exception_record，含仅留痕项）
    int          m_cachedExcTraceCount = -1;   // ★ 该值以 10s 周期刷新（避免每秒同步 DB 查询阻塞主线程）
    QLabel*      m_lblSumLocation  = nullptr;  // 分拣件数（已落格去重 EPC 数；H8 sumLocation 用此值）
    QLabel*      m_lblRfidScanCount= nullptr;  // ★ 2026-09-13 RFID 扫描次数（= RFID 推送 EPC 次数，重复计数）
    QLabel*      m_lblLastWave     = nullptr;  // 上一个波次号
    QLabel*      m_lblElapsed      = nullptr;  // ★ 2026-09-13 波次时长（mm:ss）
    // ★ 2026-09-13 超计划预警：数字（超计划的 格口+SKU 条目数）+「查看」按钮（明细弹窗）
    QLabel*      m_lblOverplanWarn = nullptr;  // 预警数量
    QPushButton* m_btnOverplanView = nullptr;  // 查看预警明细（落了几件/哪个格口容器/计划几件/多余几件）
    // ★ 2026-09-13 客户要求：波次信息面板不再显示容器绑定数据（改由第 0 页标签页展示）
    QLabel*      m_lblEfficiency   = nullptr;  // ★ 2026-09-07 分拣效率（折算件/时）
    QLabel*      m_lblPeakEff      = nullptr;  // ★ 2026-09-07 峰值效率（当日最大，件/时；落库 daily_peak）
    QPushButton* m_btnStartSorting = nullptr;  // ★ 开始分拣按钮（手动触发分拣中状态）
    QPushButton* m_btnViewException= nullptr;  // ★ 2026-09-13 「查看异常」按钮（异常数值右侧）
    // ★ 2026-09-13 需求：一键满箱回传（对当前所有已绑定容器逐个执行 H7 满箱回传；不影响波次状态机/格口启用/绑定）
    QPushButton* m_btnOneKeyFullbox= nullptr;

    // ──── 容器绑定面板 UI（业务格口，默认 6 列；异常口按 exceptionGrid 配置隐藏）────
    QWidget*     m_bindingWidget   = nullptr;  // 绑定状态容器
    QGridLayout* m_bindingGrid     = nullptr;  // 网格布局
    QLabel*      m_bindingLabels[BINDING_SLOT_COUNT] = {};    // 状态指示圆点指针（★ 2026-09-16 已隐藏，保留指针供刷新兼容）
    QLabel*      m_bindingBoxLabels[BINDING_SLOT_COUNT] = {}; // ★ 容器号标签指针（避免findChildren）
    // ★ 2026-09-16 需求⑦：整格底色 = 绑定状态（圆点去掉、文字白字），故需要外框指针与初始样式
    QVector<QFrame*> m_bindingFrames;          // 每格外框（整格底色载体）；隐藏格不创建 → 无条目
    QVector<QString> m_bindingInitialStyles;   // 与上面一一对应的初始 QSS（结束任务复位用）
    int          m_bindingCols     = 4;        // 每行列数
    int          m_bindingRows     = BINDING_SLOT_COUNT / m_bindingCols;
    QLabel*      m_lblBoundCount   = nullptr;  // 已绑定数量
    // ★ 2026-09-17 现场要求：容器绑定面板上明文提示被隐藏的异常口（"异常口：66格口"）
    QLabel*      m_lblExceptionGrid = nullptr;
    QLabel*      m_lblLockedCount  = nullptr;  // ★ 2026-09-11 已锁格数量（黄色，紧跟"已绑定"显示）
    QLabel*      m_lblUnboundCount = nullptr;  // 未绑定数量
    bool         m_bindingDirty    = false;     // ★ 绑定数据变更标记（避免无效刷新）
    // ★ 2026-09-16 需求②：该格口是否在绑定面板中隐藏（= 配置的物理异常口，如 66）
    //   口径：exceptionGrid 为空/"0" → 不隐藏任何格口（面板显示 66 格，零回归）；
    //        配置为 66 → 66 号整格不渲染（状态/容器号均不显示），面板显示 65 格。
    bool isGridHiddenInBindingPanel(int gridNum) const;
    // 面板可见格口数（隐藏异常口后 = BINDING_SLOT_COUNT - 隐藏数）；计数口径与之保持一致
    int  visibleBindingSlotCount() const;

    // ──── 波次数据记录面板 UI（★ 2026-09-06：全部已传输波次）────
    QTableWidget* m_tblWaveRecords      = nullptr;  // 波次数据记录列表
    QPushButton*  m_btnRefreshWaves     = nullptr;  // 刷新列表按钮
    QPushButton*  m_btnResumeWave       = nullptr;  // 切换选中波次按钮
    QPushButton*  m_btnViewWaveBinds    = nullptr;  // ★ 2026-09-17 查看选中波次的格口绑定（只读）
    QPushButton*  m_btnNewTask          = nullptr;  // ★ 新任务按钮（保存当前进度，清空待接收）
    // ★ 2026-09-17 绑定落库失败计数（H6 写了内存但没进 DB）：>0 时面板红字提示
    int           m_bindPersistFailedCount = 0;
    QString       m_lastBindPersistFailed;
    QLabel*       m_lblBindPersistFailed = nullptr;  // 显示在「容器绑定状态」按钮行

    // ──── 服务控制区：重传保障按钮（★ 2026-09-06 自波次面板移入）────
    QPushButton*  m_btnResendH7         = nullptr;  // 重传满箱切换(H7)
    QPushButton*  m_btnResendH8         = nullptr;  // 重传任务完结(H8)
    // ★ 2026-09-08：原"格口号输入框"改为失败格口下拉（可编辑：既能选失败记录，也能手输任意格口）
    QComboBox*    m_cmbFailedH7         = nullptr;  // H7 失败格口下拉（全部历史 失败/已取消重试）
    QComboBox*    m_cmbFailedH8         = nullptr;  // H8 失败波次下拉（全部历史 失败/已取消重试）
    QPushButton*  m_btnViewWaveQueue    = nullptr;  // ★ 查看接收波次队列（弹窗，含"接收新任务"选项）
    QVector<HttpServer::FailedFullboxItem> m_failedH7Items;  // 下拉数据快照（与下拉行一一对应）
    QVector<HttpServer::FailedEndItem>     m_failedH8Items;

    // ──── ★ 2026-09-16 现场需求④：「一键满箱回传」旁的三项计数 ────
    //   口径（tooltip 同步写明）：
    //     · 本波次满箱回传 = 当前波次已生成的 H7 报文总数（outbox_fullbox 行数：含自动满箱、
    //       手动满箱与一键回传），并显示 成功/待发/失败 拆分；
    //     · 本次一键 成功 = 本次「一键满箱回传」成功生成并入 Outbox 的报文件数（会话累计，切波次归零）；
    //     · 本次一键 失败 = ① 本次未能生成报文（Outbox 写入失败等）＋
    //                       ② 本次生成的报文最终"重试耗尽失败"的件数；
    //                       "格口无分拣记录跳过"不计入失败（沿用既有日志口径）。
    QLabel*       m_lblFullboxCount   = nullptr;  // 计数文字标签（按钮右侧）
    QString       m_fullboxCountOrder;            // 「本波次」计数所属波次（变化时归零"本次"计数）
    int           m_oneKeyBatches     = 0;        // 本次一键成功生成报文数
    int           m_oneKeyFails       = 0;        // 本次一键失败数（未生成 + 最终告警失败）
    QSet<QString> m_oneKeyMsgIds;                 // 本次一键生成的 msgId（用于失败回执归因；有界）
    void refreshFullboxCountLabel();              // 唯一取数/渲染入口（不每秒查库）
    // ★ 2026-09-16 需求④：按「效率统计」按钮的勾选状态显隐「运行日志」页右侧的效率面板
    //   （面板懒创建：首次显示时才 new；on=true 时立即刷新一次，不等 1 秒定时器）
    void applyLogEffPanelVisible(bool on);
    // ★ 2026-09-17 现场要求：效率统计面板宽度 = 「波次信息」面板宽度（上下对齐，随窗口缩放同步）
    //   由 1 秒刷新定时器调用；宽度未变时内部直接返回（零布局开销），用户拖动分隔条后不被秒级覆盖。
    void syncLogEffPanelWidth();
    QWidget* m_grpWaveInfo   = nullptr;   // 「波次信息」面板（宽度对齐的基准）
    int      m_logEffSyncedW = -1;        // 上次对齐时采用的宽度（-1 = 尚未对齐）

    // ──── 日志区 ────
    QTextEdit*   m_txtLog = nullptr;           // 运行日志文本框
    // ★ 2026-09-16 需求④：日志页右侧的「RFID 推送效率统计（当日观察）」
    //   面板类型 LogEfficiencyPanel 定义在 MainWindow.cpp 内部（不参与 moc），故：
    //     · m_logEffHost  = 分隔条右侧的**承载容器**（面板懒创建后放进来）
    //     · m_logEffPanel = 面板实例（首次显示时才 new；Null 表示尚未创建）
    //     · m_logEffTick  = "每秒一拍"回调（面板不可见时内部直接返回，零查询开销）
    //   显隐由「任务接收控制」区的「效率统计」按钮（m_btnEffChart，可勾选、默认开启）控制
    QWidget*              m_logEffHost  = nullptr;
    QWidget*              m_logEffPanel = nullptr;
    QSplitter*            m_logEffSplit = nullptr;
    std::function<void()> m_logEffTick;

    // ──── ★ 2026-09-13 实时面板：落格反馈数据（实时）────
    //   表头：序号｜时间｜EPC｜对应SKU｜格口号｜容器号｜小车号｜状态
    //   数据源：RFID 推送先建"待落格"占位行 → PLC 落格反馈到达后就地补全同一行
    QTableWidget* m_tblLive       = nullptr;   // 合并后的落格反馈实时表
    quint32       m_liveSeq       = 0;         // 行序号（本会话自增，占位创建时分配）
    QHash<QString, int> m_livePendingRows;     // EPC → 占位行号（仅主线程访问）
    QHash<QString, qint64> m_livePendingAtMs;  // EPC → 占位创建时刻（判断"超时未反馈"用）
    // ★ 2026-09-13 实时面板行操作（仅主线程调用）
    void livePanelInsertPendingRow(const QString& epc, const QStringList& cells); // RFID 先到 → 占位行
    void livePanelApplyFeedback(const QString& epc, const QStringList& cells, bool bad); // PLC 反馈 → 就地补全/新增
    void livePanelRebuildPendingIndex();       // 行裁剪后重建占位索引（行号会整体位移）
    void livePanelTrimRows();                  // 超上限裁掉最旧行
    // ★ 2026-09-15 原始报文留痕（界面）：记下每个 EPC 最近一帧的原文，供实时面板悬停显示
    void rememberLiveFrameNote(const QString& epc, const QString& epcRaw,
                               const QString& rawFrame, const QString& seq,
                               const QString& devCode);
    QHash<QString, QString> m_liveFrameNotes;   // EPC → 悬停提示文本（有界缓存）
    QList<QString>          m_liveFrameNoteOrder; // 缓存淘汰顺序（FIFO）

    // ──── ★ 2026-09-13 第二行多页窗口（标签在左侧）────
    QTabWidget*  m_tabMain = nullptr;          // 6 页：绑定状态/记录查询/波次历史/计划分配表/实时面板/运行日志

    // ──── ★ 2026-09-14 计划分配表页（独立窗口页，客户要求：不改波次面板）────
    //   口径：**一行 = 一个产品（SKU）**，横向按格口展开；
    //   每个格口一组「计划/已落/在途/余量」，类型只区分「正常分拣 / 发货」。
    //   数据源：HttpServer::planAllocSnapshot()（只读快照，锁内拷内存，不查 DB）
    //   刷新：每 2 秒只读一个版本号，版本未变或本页不在前台 → 不重建（主线程零开销）
    QTableWidget* m_tblPlanAlloc   = nullptr;   // 分配表主表
    QWidget*      m_pagePlanAlloc  = nullptr;   // ★ 本页根容器（= 加入 QTabWidget 的那个 widget）
                                                //   用于判断"本页是否在前台"：表格的直接父级是
                                                //   QGroupBox，而 QTabWidget 直接持有的是该 GroupBox，
                                                //   两者不是同一对象 —— 直接比 parentWidget() 会永假。
    QLabel*       m_lblPlanAllocSum = nullptr;  // 顶部汇总条
    QLabel*       m_lblPlanAllocHint= nullptr;  // 页脚提示（分页/截断说明）
    QLineEdit*    m_editPlanAllocSku= nullptr;  // 产品/SKU 过滤输入
    QSpinBox*     m_spinPlanAllocPage = nullptr;// 页码
    QString       m_planAllocFilter;            // 当前过滤词（空=全部）
    int           m_planAllocPage = 1;          // 当前页（1 基）
    int           m_planAllocVersion = -1;      // 上次渲染的数据版本（-1=强制首次渲染）
    qint64        m_planAllocLastRefreshMs = 0; // 上次刷新时刻（2 秒节流）
    PlanAllocSnapshot m_planAllocSnap;          // 最近一次快照（分页/过滤只重填表格，不再取数）
    void refreshPlanAllocPage(bool force = false);   // 刷新「计划分配表」页
    void renderPlanAllocRows();                      // 按当前页/过滤重填表格（不重新取快照）

    // ──── ★ 2026-09-08 水平分隔条（默认宽度分配：波次信息占首行一半）────
    QSplitter* m_rowTopInner    = nullptr;   // 第一行左半内部：任务接收控制 | 设备状态
    QSplitter* m_rowTopSplit    = nullptr;   // 第一行：左半 | 波次信息（默认 1:1）
    bool       m_defaultColSplitApplied = false;  // 默认列宽是否已按最大化宽度等分过一次

    // ──── 日志缓冲（防高频卡死） ────
    QStringList  m_logBuffer;                  // 日志消息缓冲队列
    QMutex       m_logMutex;                   // 缓冲队列互斥锁
    QTimer*      m_logFlushTimer = nullptr;    // 日志刷新定时器（100ms，高负载自动降频）
    int          m_logFlushIntervalMs = LOG_FLUSH_INTERVAL_MS; // ★ 动态调整的刷新间隔
    int          m_logDropCount       = 0;     // ★ 丢弃的日志计数（高负载时）

    // ──── 定时器 ────
    QTimer*      m_timerRefresh = nullptr;     // UI刷新定时器（每秒）

    // ──── 状态 ────
    bool         m_bRunning = false;           // ★ 任务接收状态（true=正在接收 WMS 任务下发；设备连接与此无关）
    int          m_reportRetryCount = 0;       // 手动回传重试计数（预留，当前未使用）
    QAtomicInteger<qint64> m_plcFeedbackCount{0}; // ★ PLC反馈计数（无锁，高并发安全）
    bool         m_lastTcpConnected = false;   // ★ 缓存TCP状态（避免冗余setStyleSheet）
    bool         m_lastS7Connected  = false;   // ★ 缓存S7状态
    int          m_runtimeWmsPort   = 0;       // ★ 2026-09-07 启动时实际监听的 WMS 端口（配置热更新后提示需重启项）

    // ──── 分拣记录查询 UI ────
    QComboBox*   m_cmbQueryMode     = nullptr;   // ★ 查询模式：按EPC查询 / 按SKU查询格口分配
    QLineEdit*   m_editQueryBarcode  = nullptr;   // EPC编码查询输入
    QLineEdit*   m_editQuerySku      = nullptr;   // ★ SKU编码查询输入
    QDateEdit*   m_editQueryDateFrom = nullptr;   // 查询起始日期
    QDateEdit*   m_editQueryDateTo   = nullptr;   // 查询结束日期
    QPushButton* m_btnQueryRecords   = nullptr;   // 查询按钮
    QPushButton* m_btnQueryClear     = nullptr;   // 清空结果
    QPushButton* m_btnEffChart       = nullptr;   // ★ 2026-09-07 效率统计（弹出 QCustomPlot 弹窗）
    QTableWidget* m_tblRecords       = nullptr;   // 查询结果表格
    QLabel*      m_lblRecordCount    = nullptr;   // 记录统计标签
    QLabel*      m_lblDbStats        = nullptr;   // 数据库统计标签
    QTimer*      m_dbCleanupTimer    = nullptr;   // ★ 数据库清理定时器（每日凌晨）
    QTimer*      m_stopTimeoutTimer = nullptr;   // ★ 停止超时安全网（30秒）
    QDialog*     m_effDlg            = nullptr;   // ★ 2026-09-07 效率统计弹窗实例（单例复用，关闭即删）
    QDialog*     m_excDlg            = nullptr;   // ★ 2026-09-13 异常明细弹窗实例（单例复用，关闭即删）
};
