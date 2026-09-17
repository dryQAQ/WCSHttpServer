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
#include <QTabWidget>
#include <QTabBar>
#include <QFormLayout>
#include <QGroupBox>
#include <QShowEvent>
#include <QHideEvent>
#include <QElapsedTimer>
#include <functional>      // ★ 2026-09-13 弹窗上下文注入（std::function 回调）
#include "qcustomplot.h"   // ★ 2026-09-07 效率统计图（QCustomPlot）
#include "VerticalTabBar.h" // ★ 2026-09-13 左侧标签页：中文逐字竖排自绘标签栏
#include <QSpinBox>        // ★ 2026-09-14 计划分配表：翻页控件
#include <QLineEdit>       // ★ 2026-09-14 计划分配表：过滤输入
#include <QSignalBlocker>  // ★ 2026-09-16 需求⑤：刷新失败格口下拉时阻断信号（防误触发）
#include <QClipboard>      // ★ 2026-09-17 「查看绑定」弹窗：复制只读核对 SQL

// ★ 2026-09-17 现场要求：「开始接收任务 / 结束任务」**主操作按钮放大**
//   该按钮在 4 处会改样式（开始/停止中/结束任务/复位），字号与内边距必须**逐处一致**，
//   否则切换状态时按钮尺寸会跳变 → 因此统一由本函数生成（只传背景色/悬停色）。
//   尺寸口径：最小 210×54、字号 20px（原 120×36、14px）；圆角/内边距同步放大。
static QString startStopButtonStyle(const QString& bg, const QString& hoverBg)
{
    return QString("QPushButton { background-color: %1; color: white; font-size: 20px; font-weight: bold;"
                   " border-radius: 6px; padding: 10px 28px; }"
                   "QPushButton:hover { background-color: %2; }").arg(bg, hoverBg);
}
static const int START_STOP_BTN_MIN_W = 210;   // 最小宽度（原 120）
static const int START_STOP_BTN_MIN_H = 54;    // 最小高度（原 36）

// ============================================================================
// ★ 2026-09-17「波次数据历史记录」列定义
//   第 10 列「格口绑定」= 该波次自己的绑定记录格口数（= 切回时恢复的格口数；
//   0 → 红字「无绑定记录」）。列头/取数/弹窗口径都保留，**只是默认隐藏**（现场要求）。
//   · 恢复显示：把 setupUI() 里 setColumnHidden(WAVE_RECORDS_BIND_COL, true) 的 true 改成 false；
//   · 隐藏不影响「查看绑定」按钮（只读弹窗仍在，内容与切回取数同一 SQL）。
// ============================================================================
static const int WAVE_RECORDS_COLUMN_COUNT = 10;   // 波次记录列表列数（含隐藏列）
static const int WAVE_RECORDS_BIND_COL     = 9;    // 「格口绑定」列索引（0-based）
#include <QClipboard>      // ★ 2026-09-14 计划分配表：复制为文本
#include <QTime>           // ★ 2026-09-16 需求⑨：日志页效率面板"本小时累计"（按小时归并分钟桶）

// ============================================================================
// ★ 2026-09-13 UI改版说明（第二行 = 左侧标签页多页窗口）：
//   第一行（任务接收控制 ｜ 设备状态(PLC/RFID) ｜ 波次信息）保持不变；
//   其下为一个 QTabWidget，标签置于**左侧**，共 6 页：
//     ① 容器绑定状态  ② 分拣记录查询  ③ 波次数据历史记录
//     ④ ★计划分配表  ⑤ 实时面板       ⑥ 运行日志
//   实时面板 = 合并后的「落格反馈数据（实时）」：
//     RFID 推送先占一行"待落格"，PLC 落格反馈到达后就地补全同一行（8 列）。
// ============================================================================

// ============================================================================
// ★ 2026-09-08 UI调整 实时滚动表公共逻辑：
//   新数据插到第 0 行（最新在最上，面板持续滚动不跳动），
//   超过 LIVE_TABLE_MAX_ROWS 行后自动裁掉最旧（末）行，控制内存与渲染量。
//   行数上限只影响"保留明细时长"，不影响性能：QTableView 只绘制可见行，
//   插头/删尾均为轻量操作（经现场实测 600~800ms/件 速率下占用可忽略）。
// ★ 2026-09-13：合并为单张「落格反馈数据（实时）」表；占位行索引需随裁剪重建
// ============================================================================
static const int LIVE_TABLE_MAX_ROWS = 5000;

// 实时面板占位行状态文本（用于识别"仍是占位、未被 PLC 反馈补全"）
static const char* LIVE_STATUS_PENDING = "待落格";

// ★ 2026-09-15 实时面板「原始报文悬停提示」缓存上限（件）：
//   只保留最近 N 件 EPC 的整帧原文提示，长期运行内存有界；超出按 FIFO 淘汰
static const int LIVE_FRAME_NOTE_MAX = 3000;

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

// ============================================================================
// ★ 2026-09-16 需求④：运行日志页右侧的「RFID 推送效率统计（当日观察）」面板
//
//   显隐由「任务接收控制」区的「效率统计」按钮控制（可勾选、**默认开启**）；
//   面板**按需懒创建**（首次显示时才 new）——那时 setupCore() 已建好 HttpServer，
//   不会出现"面板已建但 m_srv 还是空指针"的瞬间。
//
//   数据源与口径**完全复用**既有实现（不新增统计逻辑，避免两套口径）：
//     · 当前效率   = rfidPushPerMinute()   —— 滑动 60 秒窗口（实时）
//     · 当日峰值   = peakPerMinuteToday()  —— 当日 1 分钟窗口最高件数（跨日自动重置并落库 daily_peak）
//     · 本次累计   = rfidPushTotal()       —— 本次运行累计推送件数（跨波次不清零）
//     · 本小时累计 = 当日分钟桶里属于当前小时的桶之和（纯函数 sumMinuteBucketsOfHour）
//     · 趋势图     = efficiencySeries()    —— 最近30个整分钟桶 / 今日0~23时每小时峰值
//
//   刷新策略（避免"看效率反而拖慢主流程"）：
//     · 由主界面 1 秒刷新定时器驱动 tick()；面板不可见（切到别的标签页/按钮弹起）时直接返回
//     · 一次 efficiencySeries() 同时取"分钟桶 + 小时峰值"（同一次加锁，不会读到两个瞬间）
//   两图用按钮切换而非上下堆叠：日志页右侧是窄栏，叠两张图会被压扁看不清。
// ============================================================================

// ★ 本小时累计 = 把"当日分钟桶序列"里属于**当前小时**的桶相加
//   （序列按"旧→新"排列，最后一个桶 = 当前分钟；由 efficiencySeries(N) 给出）
//   抽成纯函数便于单独核对：这是面板唯一的派生计算。
static int sumMinuteBucketsOfHour(const QVector<int>& minuteBuckets, qint64 nowEpochMin, int hour)
{
    int sum = 0;
    for (int i = 0; i < minuteBuckets.size(); ++i)
    {
        const qint64 bucket = nowEpochMin - (minuteBuckets.size() - 1 - i);   // 旧 → 新
        if (QDateTime::fromMSecsSinceEpoch(bucket * 60000).time().hour() == hour)
            sum += minuteBuckets[i];
    }
    return sum;
}

class LogEfficiencyPanel : public QWidget
{
public:
    explicit LogEfficiencyPanel(HttpServer* srv, QWidget* parent = nullptr)
        : QWidget(parent), m_srv(srv)
    {
        QVBoxLayout* lay = new QVBoxLayout(this);
        lay->setContentsMargins(4, 4, 4, 4);
        lay->setSpacing(4);

        QLabel* title = new QLabel(QString::fromUtf8("RFID 推送效率统计（当日观察）"), this);
        title->setStyleSheet("font-size: 13px; font-weight: bold; color: #333;");
        lay->addWidget(title);

        // ── 四个关键数字（2×2）──
        QGridLayout* statGrid = new QGridLayout();
        statGrid->setHorizontalSpacing(8);
        statGrid->setVerticalSpacing(2);
        m_lblCur  = makeVal();
        m_lblPeak = makeVal();
        m_lblHour = makeVal();
        m_lblAll  = makeVal();
        statGrid->addWidget(makeCap(QString::fromUtf8("当前(1分钟)")), 0, 0);
        statGrid->addWidget(m_lblCur,  0, 1);
        statGrid->addWidget(makeCap(QString::fromUtf8("当日峰值")),    0, 2);
        statGrid->addWidget(m_lblPeak, 0, 3);
        statGrid->addWidget(makeCap(QString::fromUtf8("本小时累计")),  1, 0);
        statGrid->addWidget(m_lblHour, 1, 1);
        statGrid->addWidget(makeCap(QString::fromUtf8("本次累计")),    1, 2);
        statGrid->addWidget(m_lblAll,  1, 3);
        lay->addLayout(statGrid);

        // ── 图类型切换（窄栏下两图堆叠会看不清，改为切换）──
        QHBoxLayout* btnRow = new QHBoxLayout();
        m_btnMin = new QPushButton(QString::fromUtf8("最近30分钟"), this);
        m_btnDay = new QPushButton(QString::fromUtf8("今日0~23点"), this);
        for (QPushButton* b : {m_btnMin, m_btnDay})
        {
            b->setCheckable(true);
            b->setMinimumHeight(26);
            b->setStyleSheet(
                "QPushButton { font-size: 12px; padding: 2px 10px; border: 1px solid #bbb;"
                "  border-radius: 4px; background: #f5f5f5; color: #333; }"
                "QPushButton:checked { background: #26A96C; color: white; border-color: #1E8A57;"
                "  font-weight: bold; }");
            btnRow->addWidget(b);
        }
        m_btnMin->setChecked(true);
        lay->addLayout(btnRow);

        m_plot = new QCustomPlot(this);
        m_plot->setMinimumHeight(160);
        m_bars = new QCPBars(m_plot->xAxis, m_plot->yAxis);
        m_bars->setPen(Qt::NoPen);
        m_bars->setBrush(QColor("#26A96C"));
        m_line = m_plot->addGraph();
        m_line->setPen(QPen(QColor("#F37021"), 2));
        m_line->setAdaptiveSampling(true);
        m_plot->yAxis->setLabel(QString());
        m_plot->yAxis->setNumberFormat("f");
        m_plot->yAxis->setNumberPrecision(0);
        m_plot->xAxis->setTickLabelRotation(60);
        m_plot->xAxis->setTickLabelFont(QFont(font().family(), 8));
        m_plot->yAxis->setTickLabelFont(QFont(font().family(), 8));
        lay->addWidget(m_plot, 1);

        QLabel* tip = new QLabel(QString::fromUtf8(
            "口径：当前=滑动60秒窗口；当日峰值=当日1分钟窗口最高件数；\n"
            "本小时累计=本小时各分钟桶之和；本次累计=程序启动至今（跨波次不清零）。\n"
            "面板不可见时不刷新，不影响分拣主流程。"), this);
        tip->setStyleSheet("font-size: 11px; color: #888;");
        tip->setWordWrap(true);
        lay->addWidget(tip);

        connect(m_btnMin, &QPushButton::clicked, this, [this]() {
            m_btnDay->setChecked(false); m_btnMin->setChecked(true); refresh(); });
        connect(m_btnDay, &QPushButton::clicked, this, [this]() {
            m_btnMin->setChecked(false); m_btnDay->setChecked(true); refresh(); });

        refresh();
    }

    // 由 MainWindow 的 1 秒刷新定时器驱动；面板不可见时直接返回（零查询开销）
    void tick() { if (isVisible()) refresh(); }

private:
    QLabel* makeCap(const QString& text) const
    {
        QLabel* l = new QLabel(text, const_cast<LogEfficiencyPanel*>(this));
        l->setStyleSheet("font-size: 12px; color: #666;");
        return l;
    }
    QLabel* makeVal() const
    {
        QLabel* l = new QLabel("--", const_cast<LogEfficiencyPanel*>(this));
        l->setStyleSheet("font-size: 14px; font-weight: bold; color: #1565C0;");
        return l;
    }

    void refresh()
    {
        if (!m_srv || !m_plot) return;

        QVector<int> lastMin, hourPeaks;
        m_srv->efficiencySeries(30, &lastMin, &hourPeaks);   // 一次加锁取两组，避免读到两个瞬间

        const int perMin  = m_srv->rfidPushPerMinute();
        const int peakMin = m_srv->peakPerMinuteToday();

        // 本小时累计
        int hourSum = 0;
        {
            QVector<int> dayMin;
            m_srv->efficiencySeries(24 * 60, &dayMin, nullptr);   // 当日分钟桶（≤1440 点）
            hourSum = sumMinuteBucketsOfHour(dayMin, QDateTime::currentMSecsSinceEpoch() / 60000,
                                             QTime::currentTime().hour());
        }

        m_lblCur->setText(QString::fromUtf8("%1 件（%2 件/时）").arg(perMin).arg(perMin * 60));
        m_lblPeak->setText(QString::fromUtf8("%1 件/分（%2 件/时）").arg(peakMin).arg(peakMin * 60));
        m_lblPeak->setStyleSheet(peakMin > 0
            ? "font-size: 14px; font-weight: bold; color: #E65100;"
            : "font-size: 14px; font-weight: bold; color: #1565C0;");
        m_lblHour->setText(QString::fromUtf8("%1 件").arg(hourSum));
        m_lblAll->setText(QString::fromUtf8("%1 件").arg(m_srv->rfidPushTotal()));

        if (m_btnDay->isChecked())
        {
            // ── 今日 0~23 时：每小时峰值效率（件/时）折线 ──
            m_bars->setVisible(false);
            m_line->setVisible(true);
            QVector<double> keys(24), vals(24);
            QVector<double> ticks;
            QVector<QString> tickLabels;
            for (int h = 0; h < 24; ++h)
            {
                keys[h] = h;
                vals[h] = hourPeaks[h] * 60.0;   // 该小时峰值件数 × 60 = 件/时
                ticks << h;
                tickLabels << QString("%1点").arg(h);
            }
            m_line->setData(keys, vals);
            QSharedPointer<QCPAxisTickerText> ticker = QSharedPointer<QCPAxisTickerText>::create();
            ticker->addTicks(ticks, tickLabels);
            m_plot->xAxis->setTicker(ticker);
            m_plot->xAxis->setRange(-0.5, 23.5);
        }
        else
        {
            // ── 最近 30 分钟：每分钟推送件数柱状 ──
            m_line->setVisible(false);
            m_bars->setVisible(true);
            QVector<double> keys(lastMin.size()), vals(lastMin.size());
            QVector<double> ticks;
            QVector<QString> tickLabels;
            const qint64 nowMin = QDateTime::currentMSecsSinceEpoch() / 60000;
            for (int i = 0; i < lastMin.size(); ++i)
            {
                keys[i] = i;
                vals[i] = lastMin[i];
                ticks << i;
                tickLabels << QDateTime::fromMSecsSinceEpoch((nowMin - (lastMin.size() - 1 - i)) * 60000)
                                  .toString("HH:mm");
            }
            m_bars->setData(keys, vals);
            QSharedPointer<QCPAxisTickerText> ticker = QSharedPointer<QCPAxisTickerText>::create();
            ticker->addTicks(ticks, tickLabels);
            m_plot->xAxis->setTicker(ticker);
            m_plot->xAxis->setRange(-0.6, qMax(lastMin.size() - 0.4, 0.4));
        }
        m_plot->yAxis->rescale(true);
        const double yMax = m_plot->yAxis->range().upper;
        m_plot->yAxis->setRange(0, yMax > 0 ? yMax * 1.15 : 10.0);
        m_plot->replot(QCustomPlot::rpQueuedReplot);

        m_plot->setToolTip(QString::fromUtf8("当日观察 · %1").arg(QDate::currentDate().toString("yyyy-MM-dd")));
    }

    HttpServer*  m_srv   = nullptr;
    QCustomPlot* m_plot  = nullptr;
    QCPBars*     m_bars  = nullptr;
    QCPGraph*    m_line  = nullptr;
    QLabel*      m_lblCur  = nullptr;
    QLabel*      m_lblPeak = nullptr;
    QLabel*      m_lblHour = nullptr;
    QLabel*      m_lblAll  = nullptr;
    QPushButton* m_btnMin  = nullptr;
    QPushButton* m_btnDay  = nullptr;
};

// ============================================================================
// ★ 2026-09-13 实时面板：把一行单元格直接写到表格第 row 行（不插入新行）
//   与 livePanelInsertPendingRow / livePanelApplyFeedback 共用同一套单元格样式规则
// ============================================================================
static void writeLiveRow(QTableWidget* tbl, int row, const QStringList& cells,
                         bool warnRed, bool pendingBlue)
{
    if (!tbl || row < 0 || row >= tbl->rowCount()) return;
    const int n = qMin(cells.size(), tbl->columnCount());
    for (int c = 0; c < n; ++c)
    {
        QTableWidgetItem* it = new QTableWidgetItem(cells.at(c));
        if (c == 2) { QFont f = it->font(); f.setFamily("Consolas"); it->setFont(f); }
        it->setTextAlignment((c == 0 || c == 1) ? int(Qt::AlignHCenter | Qt::AlignVCenter)
                                               : int(Qt::AlignLeft | Qt::AlignVCenter));
        if (warnRed) it->setForeground(QColor("#D32F2F"));
        else if (pendingBlue && c == 7) it->setForeground(QColor("#1976D2"));
        tbl->setItem(row, c, it);
    }
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
// ★ 2026-09-13 EpcDetailDialog — 「EPC 全信息」弹窗
//   数据来源（全部只读查询，走 SortingDatabase 的线程局部只读连接，不占用 DB 写线程）：
//     · sorting_records   → 该 EPC 的全部分拣/落格历史（波次、格口、容器、首尾车、时间）
//     · exception_record  → 该 EPC 的全部异常留痕（是否计入处理数、是否已闭环）
//     · return_wave_item  → 该 SKU 的计划格口/计划数量（计划视角对照）
//   入口：波次信息「查看异常」弹窗双击行；分拣记录查询（按EPC/按SKU/按格口）双击行
// ============================================================================
class EpcDetailDialog : public QDialog
{
public:
    EpcDetailDialog(SortingDatabase* db, const QString& epc, QWidget* parent = nullptr)
        : QDialog(parent), m_db(db), m_epc(epc)
    {
        setWindowTitle(QString::fromUtf8("EPC 全信息 — %1").arg(epc));
        resize(1080, 720);

        QVBoxLayout* root = new QVBoxLayout(this);

        // ── 字段区：一眼看全该 EPC 的当前状态 ──
        QGroupBox* grpInfo = new QGroupBox(QString::fromUtf8("基本信息"));
        QFormLayout* form = new QFormLayout(grpInfo);
        form->setLabelAlignment(Qt::AlignRight);
        auto addField = [&](const QString& title, QLabel*& out) {
            out = new QLabel("--");
            out->setStyleSheet("font-size: 13px; font-weight: bold; color: #1565C0;");
            out->setTextInteractionFlags(Qt::TextSelectableByMouse);
            form->addRow(new QLabel(title), out);
        };
        addField(QString::fromUtf8("EPC："),      m_lblEpc);
        addField(QString::fromUtf8("对应SKU："),  m_lblSku);
        addField(QString::fromUtf8("所属波次："), m_lblOrder);
        addField(QString::fromUtf8("实际落格号："), m_lblGrid);
        addField(QString::fromUtf8("计划格口/数量："), m_lblPlan);
        addField(QString::fromUtf8("容器号："),   m_lblBox);
        addField(QString::fromUtf8("小车号："),   m_lblCar);
        addField(QString::fromUtf8("分拣时间："), m_lblTime);
        addField(QString::fromUtf8("状态："),     m_lblStatus);
        addField(QString::fromUtf8("异常："),     m_lblExc);

        // ── 三张明细表 ──
        QTabWidget* tabs = new QTabWidget(this);

        m_tblSorted = new QTableWidget();
        styleDetailTable(m_tblSorted, QStringList()
            << "序号" << "波次号" << "格口号" << "容器号" << "首车" << "尾车" << "件数" << "库位" << "分拣时间");
        m_tblSorted->setToolTip(QString::fromUtf8("sorting_records：该 EPC 全部落格实绩（受\"同波次同 EPC 防重\"保护）"));
        tabs->addTab(m_tblSorted, QString::fromUtf8("分拣/落格历史"));

        m_tblExc = new QTableWidget();
        styleDetailTable(m_tblExc, QStringList()
            << "序号" << "发生时间" << "异常类型" << "SKU" << "原因" << "是否计入处理数" << "闭环状态");
        tabs->addTab(m_tblExc, QString::fromUtf8("异常留痕"));

        m_tblPlan = new QTableWidget();
        styleDetailTable(m_tblPlan, QStringList()
            << "序号" << "波次号" << "计划格口" << "格口类型" << "计划数量" << "已分拣数量" << "库位" << "容器号(WMS)");
        tabs->addTab(m_tblPlan, QString::fromUtf8("计划明细"));

        // ★ 2026-09-15 需求②：该 EPC 的全部 RFID 原始推送帧（识别前后 + 整帧原文）
        m_tblRaw = new QTableWidget();
        styleDetailTable(m_tblRaw, QStringList()
            << "序号" << "落库时间" << "识别后EPC" << "识别前EPC原文" << "流水号" << "设备编码"
            << "小车号" << "字节数" << "整帧原始报文");
        m_tblRaw->setToolTip(QString::fromUtf8(
            "rfid_raw：该 EPC 的全部原始推送帧（可按识别前后 EPC 双通道命中）\n"
            "同一 EPC 多行 = 双读/重扫重投；NOREAD 行 = 未读到标签（仅留痕，不进入分拣）\n"
            "「整帧原始报文」列可选中复制，用于与推送侧逐字节核对"));
        tabs->addTab(m_tblRaw, QString::fromUtf8("RFID原始报文"));

        root->addWidget(grpInfo);
        root->addWidget(tabs, 1);

        QDialogButtonBox* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
        box->button(QDialogButtonBox::Close)->setText(QString::fromUtf8("关闭"));
        connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(box);

        load();
    }

private:
    static void styleDetailTable(QTableWidget* t, const QStringList& headers)
    {
        t->setColumnCount(headers.size());
        t->setHorizontalHeaderLabels(headers);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->setSelectionMode(QAbstractItemView::SingleSelection);
        t->setAlternatingRowColors(true);
        t->verticalHeader()->setVisible(false);
        t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        t->horizontalHeader()->setStretchLastSection(true);
        t->horizontalHeader()->setDefaultSectionSize(120);
        t->verticalHeader()->setDefaultSectionSize(28);
        t->setStyleSheet(
            "QTableWidget { font-size: 13px; }"
            "QTableWidget::item { padding: 3px 6px; }"
            "QHeaderView::section { background-color: #e0e0e0; font-weight: bold; padding: 5px; }");
    }

    // 是否计入"异常数"（只有 PLC 主动判定失败才计入；其余为仅留痕）
    static bool isCountedException(const QString& type)
    {
        return type == "plc_no_grid" || type == "plc_info_incomplete"
            || type == QString::fromUtf8("无格口") || type == QString::fromUtf8("信息不全");
    }

    void fill(QTableWidget* t, const QStringList& cells)
    {
        const int row = t->rowCount();
        t->insertRow(row);
        for (int c = 0; c < cells.size() && c < t->columnCount(); ++c)
        {
            QTableWidgetItem* it = new QTableWidgetItem(cells.at(c));
            if (c == 0) it->setTextAlignment(Qt::AlignCenter);
            t->setItem(row, c, it);
        }
    }

    void load()
    {
        if (!m_db) return;

        // ── ① 分拣/落格历史 ──
        //   ★ 2026-09-16 需求③：EPC 全信息窗展示该 EPC 的**完整历史**（不受"分拣记录查询"页的
        //     日期区间约束），故传空 QDateTime = 不加日期条件（DB 层对该侧不做过滤）
        const QVector<SortingRecord> recs = m_db->queryByBarcode(m_epc, QDateTime(), QDateTime(),
                                                                 SORTING_QUERY_MAX_RESULTS);
        for (int i = 0; i < recs.size(); ++i)
        {
            const SortingRecord& r = recs[i];
            fill(m_tblSorted, QStringList()
                << QString::number(i + 1) << r.orderCode
                << (r.gridNum.isEmpty() ? "--" : gridKeyOf(r.gridNum))
                << (r.boxcode.isEmpty() ? "--" : r.boxcode)
                << (r.firstCar.isEmpty() ? r.carNum : r.firstCar)
                << (r.lastCar.isEmpty() ? "--" : r.lastCar)
                << QString::number(r.gridCount)
                << (r.volu.isEmpty() ? "--" : r.volu)
                << r.sortTime);
        }
        if (recs.size() >= SORTING_QUERY_MAX_RESULTS)
            fill(m_tblSorted, QStringList() << "…" << QString::fromUtf8("已达单次查询上限，可能截断"));

        // ── ② 异常留痕 ──
        const QVector<ExceptionRecord> excs = m_db->queryExceptions(QString(), m_epc, QString(), QString(), QString(),
                                                                   SORTING_QUERY_MAX_RESULTS);
        for (int i = 0; i < excs.size(); ++i)
        {
            const ExceptionRecord& e = excs[i];
            fill(m_tblExc, QStringList()
                << QString::number(i + 1) << e.time << e.type
                << (e.sku.isEmpty() ? "--" : e.sku)
                << e.reason
                << (isCountedException(e.type) ? QString::fromUtf8("计入处理") : QString::fromUtf8("仅留痕"))
                << (e.handled ? QString::fromUtf8("已闭环(该EPC后已成功落格)")
                              : QString::fromUtf8("未闭环")));
        }

        // ── ③ 字段区汇总 ──
        m_lblEpc->setText(m_epc);
        if (!recs.isEmpty())
        {
            const SortingRecord& r0 = recs.first();
            m_lblSku->setText(r0.sku.isEmpty() ? QString::fromUtf8("--（落格时未取到 SKU）") : r0.sku);
            m_lblOrder->setText(r0.orderCode);
            const QString gk = gridKeyOf(r0.gridNum);
            m_lblGrid->setText(QString("%1（WMS编码 %2）").arg(gk, gridToWmsCode(gk)));
            m_lblBox->setText(r0.boxcode.isEmpty() ? "--" : r0.boxcode);
            m_lblCar->setText(QString::fromUtf8("首车:%1  尾车:%2")
                .arg(r0.firstCar.isEmpty() ? r0.carNum : r0.firstCar,
                     r0.lastCar.isEmpty() ? "--" : r0.lastCar));
            m_lblTime->setText(r0.sortTime);
            m_lblStatus->setText(QString::fromUtf8("已落格（本 EPC 共 %1 条落格记录）").arg(recs.size()));

            // 计划对照（同 SKU + 同波次）
            QVector<ReturnWaveItemRecord> plans;
            for (const ReturnWaveItemRecord& it : m_db->querySkuGridMapping(r0.sku))
            {
                if (it.orderCode == r0.orderCode) plans.append(it);
            }
            for (int i = 0; i < plans.size(); ++i)
            {
                const ReturnWaveItemRecord& it = plans[i];
                QString typeText = it.gridType;
                if (it.gridType == "0") typeText = QString::fromUtf8("分类");
                else if (it.gridType == "1") typeText = QString::fromUtf8("异常");
                else if (it.gridType == "2") typeText = QString::fromUtf8("发货");
                fill(m_tblPlan, QStringList()
                    << QString::number(i + 1) << it.orderCode << gridKeyOf(it.gridNum)
                    << typeText << QString::number(it.planQty) << QString::number(it.sortedQty)
                    << (it.volu.isEmpty() ? "--" : it.volu)
                    << (it.obxCode.isEmpty() ? "--" : it.obxCode));
            }
            QStringList planTexts;
            for (const ReturnWaveItemRecord& it : plans)
                planTexts << QString("%1(计划%2)").arg(gridKeyOf(it.gridNum)).arg(it.planQty);
            m_lblPlan->setText(planTexts.isEmpty()
                ? QString::fromUtf8("--（本波次未下发该 SKU 的计划格口）")
                : planTexts.join(QString::fromUtf8("、")));
        }
        else
        {
            m_lblStatus->setText(QString::fromUtf8("无落格记录（可能仅异常留痕，或未进入分拣）"));
            m_lblOrder->setText(excs.isEmpty() ? "--" : excs.first().orderCode);
            m_lblSku->setText(excs.isEmpty() || excs.first().sku.isEmpty() ? "--" : excs.first().sku);
            // 无落格记录时用计划明细反查该 EPC 所属 SKU 的计划
            if (!m_lblSku->text().isEmpty() && m_lblSku->text() != "--")
            {
                const QVector<ReturnWaveItemRecord> plans = m_db->querySkuGridMapping(m_lblSku->text());
                for (int i = 0; i < plans.size(); ++i)
                {
                    const ReturnWaveItemRecord& it = plans[i];
                    fill(m_tblPlan, QStringList()
                        << QString::number(i + 1) << it.orderCode << gridKeyOf(it.gridNum)
                        << it.gridType << QString::number(it.planQty) << QString::number(it.sortedQty)
                        << (it.volu.isEmpty() ? "--" : it.volu)
                        << (it.obxCode.isEmpty() ? "--" : it.obxCode));
                }
            }
        }

        // 异常汇总（字段区）
        if (!excs.isEmpty())
        {
            QStringList parts;
            int counted = 0;
            for (const ExceptionRecord& e : excs)
            {
                if (isCountedException(e.type)) ++counted;
            }
            parts << QString::fromUtf8("留痕 %1 条（其中计入处理数 %2 条）").arg(excs.size()).arg(counted);
            parts << QString::fromUtf8("最新：%1 — %2").arg(excs.first().type, excs.first().reason);
            m_lblExc->setText(parts.join(QString::fromUtf8("；")));
            m_lblExc->setStyleSheet("font-size: 13px; font-weight: bold; color: #D32F2F;");
        }
        else
        {
            m_lblExc->setText(QString::fromUtf8("无异常留痕"));
            m_lblExc->setStyleSheet("font-size: 13px; font-weight: bold; color: #2E7D32;");
        }

        // ── ④ ★ 2026-09-15 RFID 原始推送帧（需求②：保留原始报文，可按识别前后 EPC 双通道回查）──
        {
            const QVector<RfidRawRecord> raws = m_db->queryRfidRawByEpc(m_epc, 200);
            for (int i = 0; i < raws.size(); ++i)
            {
                const RfidRawRecord& r = raws[i];
                fill(m_tblRaw, QStringList()
                    << QString::number(i + 1)
                    << r.time
                    << (r.epc.isEmpty()
                            ? (r.noread ? QString::fromUtf8("（NOREAD 未读到标签）") : "--")
                            : r.epc)
                    << (r.epcRaw.isEmpty() ? r.epc : r.epcRaw)
                    << (r.seq.isEmpty() ? QStringLiteral("-") : r.seq)
                    << (r.devCode.isEmpty() ? QStringLiteral("-") : r.devCode)
                    << (r.carNum.isEmpty() ? QStringLiteral("-") : r.carNum)
                    << QString::number(r.bytes)
                    << (r.rawFrame.isEmpty() ? QString::fromUtf8("（无整帧原文：HTTP 直推入口）") : r.rawFrame));
            }
            if (raws.isEmpty())
            {
                fill(m_tblRaw, QStringList()
                    << "--" << "--" << m_epc << "--" << "--" << "--" << "--" << "--"
                    << QString::fromUtf8("（该 EPC 无原始报文明细：可能为改造前的历史数据，"
                                         "或原始报文已超保留期 %1 天被清理）").arg(RFID_RAW_RETAIN_DAYS));
            }
        }
    }

private:
    SortingDatabase* m_db  = nullptr;
    QString          m_epc;
    QLabel*          m_lblEpc = nullptr;
    QLabel*          m_lblSku = nullptr;
    QLabel*          m_lblOrder = nullptr;
    QLabel*          m_lblGrid = nullptr;
    QLabel*          m_lblPlan = nullptr;
    QLabel*          m_lblBox = nullptr;
    QLabel*          m_lblCar = nullptr;
    QLabel*          m_lblTime = nullptr;
    QLabel*          m_lblStatus = nullptr;
    QLabel*          m_lblExc = nullptr;
    QTableWidget*    m_tblSorted = nullptr;
    QTableWidget*    m_tblExc = nullptr;
    QTableWidget*    m_tblPlan = nullptr;
    QTableWidget*    m_tblRaw  = nullptr;   // ★ 2026-09-15 RFID 原始推送帧（rfid_raw）
};

// ============================================================================
// ★ 2026-09-13 ExceptionListDialog — 「查看处理」弹窗（波次信息「处理」右侧按钮）
//   回答"到底哪些件还在异常口/有过什么异常"：
//     · 顶部：计划/已分拣/处理/异常口/异常留痕 的换算说明（口径显式化）
//     · 表格：本波次全部异常留痕（含仅留痕项），标注"是否计入处理数"与"闭环状态"
//     · 双击行 → EpcDetailDialog 查看该 EPC 全信息
//   ★ 客户口径：面板「处理」与「异常口」是同一个量（仍未处理完的件数），成功落格即递减
// ============================================================================
class ExceptionListDialog : public QDialog
{
public:
    ExceptionListDialog(SortingDatabase* db, const QString& orderCode, QWidget* parent = nullptr)
        : QDialog(parent), m_db(db)
    {
        setWindowTitle(QString::fromUtf8("异常明细 — 波次 %1").arg(orderCode.isEmpty() ? QString::fromUtf8("(全部)") : orderCode));
        resize(1180, 680);

        QVBoxLayout* root = new QVBoxLayout(this);

        // ── 顶部：口径说明（一行看懂"异常"到底记什么）──
        m_lblSummary = new QLabel();
        m_lblSummary->setWordWrap(true);
        m_lblSummary->setStyleSheet("font-size: 13px; color: #333; background: #FFF8E1;"
                                    " border: 1px solid #FFE082; border-radius: 4px; padding: 8px;");
        root->addWidget(m_lblSummary);

        // ── 波次切换 ──
        QHBoxLayout* condRow = new QHBoxLayout();
        condRow->addWidget(new QLabel(QString::fromUtf8("波次：")));
        m_cmbWave = new QComboBox();
        m_cmbWave->setMinimumWidth(260);
        m_cmbWave->setFont(QFont(font().family(), 13));
        m_cmbWave->addItem(QString::fromUtf8("(全部波次)"), QString());
        if (db)
        {
            for (const WaveRecordProgress& w : db->getAllWaves())
                m_cmbWave->addItem(QString("%1  [%2]").arg(w.orderCode, WaveSnapshot::statusToString(w.status)), w.orderCode);
        }
        const int idx = m_cmbWave->findData(orderCode);
        m_cmbWave->setCurrentIndex(idx >= 0 ? idx : 0);
        condRow->addWidget(m_cmbWave);
        condRow->addStretch();

        m_btnExport = new QPushButton(QString::fromUtf8("导出到运行日志"));
        m_btnExport->setMinimumHeight(32);
        m_btnExport->setFont(QFont(font().family(), 13));
        condRow->addWidget(m_btnExport);
        root->addLayout(condRow);

        // ── 明细表 ──
        m_tbl = new QTableWidget();
        m_tbl->setColumnCount(8);
        m_tbl->setHorizontalHeaderLabels(QStringList()
            << "序号" << "发生时间" << "异常类型" << "EPC" << "对应SKU" << "原因"
            << "是否计入处理数" << "闭环状态");
        m_tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_tbl->setSelectionMode(QAbstractItemView::SingleSelection);
        m_tbl->setAlternatingRowColors(true);
        m_tbl->verticalHeader()->setVisible(false);
        m_tbl->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_tbl->horizontalHeader()->setStretchLastSection(true);
        m_tbl->verticalHeader()->setDefaultSectionSize(28);
        m_tbl->setStyleSheet(
            "QTableWidget { font-size: 13px; }"
            "QTableWidget::item { padding: 3px 6px; }"
            "QHeaderView::section { background-color: #e0e0e0; font-weight: bold; padding: 5px; }");
        root->addWidget(m_tbl, 1);

        QDialogButtonBox* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
        box->button(QDialogButtonBox::Close)->setText(QString::fromUtf8("关闭"));
        connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(box);

        connect(m_cmbWave, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { reload(); });
        connect(m_tbl, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
            QTableWidgetItem* it = m_tbl->item(row, 3);   // EPC 列
            if (!it || it->text().isEmpty() || it->text() == "--") return;
            EpcDetailDialog dlg(m_db, it->text(), this);
            dlg.exec();
        });
        connect(m_btnExport, &QPushButton::clicked, this, [this]() { exportToLog(); });

        reload();
    }

private:
    void reload()
    {
        if (!m_db) return;
        m_orderCode = m_cmbWave->currentData().toString();

        m_rows = m_db->queryExceptions(m_orderCode, QString(), QString(), QString(), QString(),
                                       SORTING_QUERY_MAX_RESULTS);
        m_tbl->setRowCount(0);
        m_tbl->setRowCount(m_rows.size());

        int counted = 0, resolved = 0;
        for (int i = 0; i < m_rows.size(); ++i)
        {
            const ExceptionRecord& e = m_rows[i];
            const bool isCounted = (e.type == "plc_no_grid" || e.type == "plc_info_incomplete");
            if (isCounted) ++counted;
            if (e.handled) ++resolved;

            auto setCell = [&](int col, const QString& text, const QColor& color = QColor(), bool center = false) {
                QTableWidgetItem* it = new QTableWidgetItem(text);
                if (center) it->setTextAlignment(Qt::AlignCenter);
                if (color.isValid()) it->setForeground(color);
                m_tbl->setItem(i, col, it);
                return it;
            };
            setCell(0, QString::number(i + 1), QColor(), true);
            setCell(1, e.time);
            setCell(2, e.type, isCounted ? QColor("#D32F2F") : QColor("#616161"));
            setCell(3, e.epc.isEmpty() ? "--" : e.epc);
            setCell(4, e.sku.isEmpty() ? "--" : e.sku);
            setCell(5, e.reason);
            setCell(6, isCounted ? QString::fromUtf8("计入") : QString::fromUtf8("仅留痕"), QColor(), true);
            setCell(7, e.handled ? QString::fromUtf8("已闭环（该 EPC 后已成功落格）")
                                 : QString::fromUtf8("未闭环"), QColor(), false);
        }

        // 与内存实时计数对照（同一波次时才有意义）
        QString memText = QString::fromUtf8("--");
        if (m_srv && m_srv->waveManager() && !m_orderCode.isEmpty()
            && m_srv->waveManager()->orderCode() == m_orderCode)
        {
            memText = QString::fromUtf8("处理/异常口 %1 件（仍在异常口、未处理完，去重 EPC）｜已分拣 %2 件次")
                .arg(m_srv->waveManager()->exception())
                .arg(m_srv->waveManager()->sorted());
        }
        m_lblSummary->setTextFormat(Qt::RichText);   // ★ 说明文字含 <b>/<u> 强调，需按富文本渲染
        m_lblSummary->setText(QString::fromUtf8(
            "口径说明：\n"
            "  · 处理（波次面板）= <b>仍在异常口、尚未处理完的件数（去重 EPC）</b>——某 EPC 掉入异常口即 +1，"
            "该 EPC 之后<u>成功落格</u>即视为已处理，立即 −1（不会一直保留）。\n"
            "  · 异常口（波次面板）= <b>与「处理」同一个量</b>（同一批待处理件，去重 EPC）；成功落格时同步减少。\n"
            "  · 异常留痕（下表）= exception_record 记录条数：其中只有 PLC 主动判定失败的 "
            "plc_no_grid / plc_info_incomplete <b>计入处理数</b>；\n"
            "    未匹配/未绑定/冲突/重扫超限/发送超时等属于<u>仅留痕</u>（PLC 报成功、已计已分拣），不增加处理数。\n"
            "  · 本波次实时对照：%1    ｜ 当前筛选：留痕 %2 条（计入处理 %3 条，已闭环 %4 条）%5")
            .arg(memText).arg(m_rows.size()).arg(counted).arg(resolved)
            .arg(m_rows.size() >= SORTING_QUERY_MAX_RESULTS
                     ? QString::fromUtf8("　⚠ 已达单次查询上限 %1 条，可能截断").arg(SORTING_QUERY_MAX_RESULTS)
                     : QString()));
    }

    void exportToLog()
    {
        if (m_rows.isEmpty()) return;
        QStringList lines;
        lines << QString::fromUtf8("[异常明细] 波次=%1 共 %2 条")
            .arg(m_orderCode.isEmpty() ? QString::fromUtf8("(全部)") : m_orderCode).arg(m_rows.size());
        for (int i = 0; i < m_rows.size(); ++i)
        {
            const ExceptionRecord& e = m_rows[i];
            lines << QString("%1. %2 [%3] EPC=%4 SKU=%5 %6 | %7")
                .arg(i + 1).arg(e.time, e.type)
                .arg(e.epc.isEmpty() ? "--" : e.epc)
                .arg(e.sku.isEmpty() ? "--" : e.sku)
                .arg(e.handled ? QString::fromUtf8("[已闭环]") : QString::fromUtf8("[未闭环]"))
                .arg(e.reason);
        }
        if (m_logCb) m_logCb(lines.join("\n"));
    }

public:
    // 主窗口注入：内存实时计数来源 + 日志导出回调（避免弹窗直接依赖 MainWindow 私有成员）
    void setContext(HttpServer* srv, std::function<void(const QString&)> logCb)
    {
        m_srv = srv;
        m_logCb = std::move(logCb);
    }

private:
    SortingDatabase* m_db  = nullptr;
    HttpServer*      m_srv = nullptr;
    QString          m_orderCode;
    QVector<ExceptionRecord> m_rows;
    QLabel*       m_lblSummary = nullptr;
    QComboBox*    m_cmbWave = nullptr;
    QPushButton*  m_btnExport = nullptr;
    QTableWidget* m_tbl = nullptr;
    std::function<void(const QString&)> m_logCb;
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

    // ★ 2026-09-16 需求④：「效率统计」按钮**默认开启** —— 面板是懒创建的，而 setChecked(true)
    //   发生在 setupUI 期间（那一刻 HttpServer 还没建），故这里在 setupCore() 之后显式应用一次，
    //   确保"开机即在运行日志页右侧显示效率面板"
    applyLogEffPanelVisible(m_btnEffChart && m_btnEffChart->isChecked());

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
                ? QString("正在接收任务，确定退出吗？\n\n"
                          "① 当前波次将被「切出」保存：进度/明细/计划/落格记录保留在数据库，\n"
                          "   格口绑定状态一并归档留痕（可追溯）；\n"
                          "② 下次打开软件为「新任务状态」（格口全部未绑定）；\n"
                          "③ 需要继续上次任务时，从「波次数据记录」选中该波次点「切换」即可恢复。\n\n"
                          "（建议先点击\"结束任务\"完成完结回传；退出后设备连接将断开）")
                : QString("正在停止接收（完结回传未完成），确定退出吗？\n"
                          "（H8 未确认消息将在下次开始接收时自动补传）"),
            QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes)
        {
            event->ignore();
            return;
        }
        // ★ 2026-09-15 需求①：关闭即切出当前波次 —— 保留全部信息与格口绑定状态到数据库便于回溯；
        //   下次开启为新任务状态（清空格口绑定关系）。放在停止收尾之前执行，
        //   保证"归档的是关掉那一刻的绑定映射"。
        //   异常关闭（崩溃/断电/强杀）不走此处 → 由下次启动 restoreWaveFromDB() 兜底归档。
        if (m_pServer)
        {
            appendLog("[切出] 关闭软件：正在切出当前波次并归档格口绑定状态（数据保留于数据库，可回溯/可切回）");
            m_pServer->switchOutWaveForExit();
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
// ★ 2026-09-08 UI调整：默认最大化后，把第一行水平分隔条按"两大部分各占一半"
//   布置一次（第一行 = 左半(任务接收控制|设备状态) | 波次信息，各占整行一半）。
//   在 changeEvent 收到 WindowStateChange 且窗口已最大化时触发（此时几何已确定），
//   只执行一次，之后仍可手动拖动分隔条。
// ★ 2026-09-13 UI改版：第二行改为左侧标签页（QTabWidget），
//   原第二/三/四行的水平分隔条已不存在，此处只处理第一行。
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
    // m_rowTopInner 不强制等分：任务接收控制保持内容宽度，设备状态吃满左半余量
    // 第二行（标签页）不需要列宽分配；垂直比例由 vsplit->setSizes 给定
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
    //   ★ 2026-09-17 现场要求：本按钮放大（主操作按钮）——尺寸/字号口径见 startStopButtonStyle()
    m_btnStartStop = new QPushButton(QCoreApplication::translate("MainWindow", "开始接收任务"));
    m_btnStartStop->setMinimumWidth(START_STOP_BTN_MIN_W);
    m_btnStartStop->setMinimumHeight(START_STOP_BTN_MIN_H);
    m_btnStartStop->setStyleSheet(startStopButtonStyle("#4CAF50", "#45a049"));

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

    // ★ 2026-09-13 需求：在「开始/结束接收任务」旁新增「一键满箱回传」
    //   点击 → 对**当前所有已绑定容器**逐个按 H7 满箱回传上传（有分拣记录的格口才发，空格口跳过并提示）。
    //   ★ 不影响其他功能：复用 manualFullbox → sendFullboxForGrid，不动波次状态机、不禁用格口、不改绑定；
    //     未成功的报文照常保留在 Outbox（可重传），失败不会阻塞分拣。
    m_btnOneKeyFullbox = new QPushButton(QCoreApplication::translate("MainWindow", "一键满箱回传"));
    m_btnOneKeyFullbox->setMinimumWidth(120);
    m_btnOneKeyFullbox->setMinimumHeight(32);
    m_btnOneKeyFullbox->setStyleSheet(
        "QPushButton { background-color: #00897B; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #00796B; }"
        "QPushButton:disabled { background-color: #BDBDBD; }");
    m_btnOneKeyFullbox->setToolTip(QCoreApplication::translate("MainWindow",
        "对当前所有已绑定容器逐个执行 H7 满箱回传（同一批，自动统计）\n"
        "有分拣记录的格口才发送；无记录的格口跳过\n"
        "不影响波次状态机与格口启用状态，失败报文保留在 Outbox 可重传"));
    // ★ 2026-09-16 现场需求④：按钮右侧显示三项计数（本波次满箱回传次数 / 本次一键回传次数 / 本次一键失败次数）
    m_lblFullboxCount = new QLabel();
    m_lblFullboxCount->setStyleSheet("font-size: 12px; color: #555; padding: 0 4px;");
    m_lblFullboxCount->setText(QString::fromUtf8("本波次满箱回传 0 次 ｜ 本次一键 成功 0 / 失败 0"));
    m_lblFullboxCount->setToolTip(QCoreApplication::translate("MainWindow",
        "口径说明：\n"
        "  本波次满箱回传 = 当前波次已生成的 H7 满箱报文总数（含自动满箱、手动满箱与一键回传），\n"
        "                   括号内为 成功/待发/失败 拆分；实时取自 outbox_fullbox\n"
        "  本次一键 成功   = 本次「一键满箱回传」成功生成并入 Outbox 的报文件数（无记录格口跳过，不计入）\n"
        "  本次一键 失败   = 本次未能生成报文（Outbox 写入失败等）＋ 本次生成的报文最终失败（重试耗尽）的件数；\n"
        "                   失败报文保留在 Outbox 可重传，分拣不受影响\n"
        "  切换波次时「本次一键」计数自动归零，「本波次」数值随之切换到新波次"));
    {
        QHBoxLayout* oneKeyRow = new QHBoxLayout();
        oneKeyRow->addStretch();
        oneKeyRow->addWidget(m_btnOneKeyFullbox);
        oneKeyRow->addWidget(m_lblFullboxCount);   // ★ 需求④：计数文字紧跟按钮
        oneKeyRow->addStretch();
        serverLayout->addLayout(oneKeyRow);
    }

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
            QCoreApplication::translate("MainWindow", "选择本波次失败格口，或手输格口号"));
    // ★ 2026-09-16 现场需求⑤：下拉只列**当前运行波次**的失败/已取消 H7（不再混入其它波次历史）
    m_cmbFailedH7->setToolTip(QCoreApplication::translate("MainWindow",
        "下拉=**当前运行波次**的失败/已取消满箱报文（格口·波次·失败条数）：选中后点按钮按该格口精确重传；\n"
        "无事例时显示「本波次暂无失败记录」（无运行波次时显示「暂无运行波次」）\n"
        "也可直接手输格口号：对当前波次该格口的分拣记录生成新的 H7 满箱回传并上传"));

    // ★ 2026-09-16 现场需求①：终态行（已完成/已取消）选中时禁用「切换选中波次」并给出正确入口提示。
    //   说明：本连接放在表控件创建之后（「切换选中波次」按钮在下一段"波次数据历史记录"页创建），
    //   故此处用指针判空的 lambda，运行时始终读取最新指针。见下方 setupUI 末尾的补充连接。

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
    // ══════════════════════════════════════════════════════════════════════════
    // ★ 2026-09-16 需求③④：「效率统计」按钮（原在「分拣记录查询」页条件行）
    //   · 位置：任务接收控制区，与「查看接收波次队列」「设置配置」同一水平行，
    //           **位于「设置配置」左侧**
    //   · 行为：**可勾选、默认开启（按下态）**；点击 = 显示/隐藏「运行日志」页右侧的
    //           「RFID 推送效率统计（当日观察）」面板（面板按需懒创建，见日志页）
    //   · 口径与数据源完全复用既有统计接口（rfidPushPerMinute / peakPerMinuteToday /
    //     efficiencySeries / rfidPushTotal），不新增第二套统计逻辑
    // ══════════════════════════════════════════════════════════════════════════
    m_btnEffChart = new QPushButton(QCoreApplication::translate("MainWindow", "效率统计"));
    m_btnEffChart->setCheckable(true);
    m_btnEffChart->setChecked(true);        // ★ 默认为"开启"（按下态）
    m_btnEffChart->setMinimumHeight(30);
    m_btnEffChart->setStyleSheet(
        "QPushButton { background-color: #f5f5f5; color: #333; font-size: 12px; font-weight: bold;"
        "  border: 1px solid #26A96C; border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #e8f5ef; }"
        "QPushButton:checked { background-color: #26A96C; color: white; }");
    m_btnEffChart->setToolTip(QCoreApplication::translate("MainWindow",
        "显示/隐藏「运行日志」页右侧的 RFID 推送效率统计面板（当日观察）：\n"
        "  · 默认开启（按钮按下 = 面板显示）\n"
        "  · 面板内容：当前(1分钟)/当日峰值/本小时累计/本次累计 + 最近30分钟柱状 / 今日0~23点折线\n"
        "  · 关闭后日志区自动吃满宽度；面板不可见时不刷新，不影响分拣主流程"));
    connect(m_btnEffChart, &QPushButton::toggled, this, &MainWindow::applyLogEffPanelVisible);

    // ── 第1行：查看接收波次队列 + 效率统计 + 设置配置 + 新任务（同一水平行）──
    //   ★ 2026-09-17 现场要求：「新任务」按钮从「波次数据历史记录」页**移到「设置配置」旁边**
    //     （任务接收控制区同一行，紧邻「设置配置」右侧）——现场把"开新任务"当日常主操作，
    //      放这里伸手可及，不用先切到波次记录页。
    //     行为完全不变：仍调用 onStartNewWaveTask()（保存当前波次进度/数据 → 内存清空回空闲）。
    m_btnNewTask = new QPushButton(QCoreApplication::translate("MainWindow", "新任务"));
    m_btnNewTask->setMinimumHeight(34);
    m_btnNewTask->setFont(QFont(font().family(), 13));
    m_btnNewTask->setStyleSheet(
        "QPushButton { background-color: #FF5722; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #E64A19; }");
    m_btnNewTask->setToolTip(QCoreApplication::translate("MainWindow",
        "开始新任务：保存当前波次的进度与全部数据（保留于数据库，可随时从「波次数据历史记录」切换回来），\n"
        "清空内存回到空闲状态等待接收新波次；当前波次仍在作业中时会先要求点「结束任务」。"));

    QHBoxLayout* viewQueueRow = new QHBoxLayout();
    viewQueueRow->addStretch();
    viewQueueRow->addWidget(m_btnViewWaveQueue);
    viewQueueRow->addWidget(m_btnEffChart);   // ★ 需求③：效率统计在「设置配置」左边
    viewQueueRow->addWidget(btnSettings);     // ★ 2026-09-08「设置配置」
    viewQueueRow->addWidget(m_btnNewTask);    // ★ 2026-09-17「新任务」紧邻「设置配置」
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
    // ★ 2026-09-13 需求：一键满箱回传（对所有已绑定容器逐个执行 H7 满箱回传；不影响其他功能）
    if (m_btnOneKeyFullbox)
        connect(m_btnOneKeyFullbox, &QPushButton::clicked, this, &MainWindow::onOneKeyFullbox);
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
    // ★ 2026-09-17 现场要求：「运行日志」页右侧的**效率统计面板宽度与「波次信息」面板相同**（上下对齐）
    //   —— 记下波次信息面板指针，供 syncLogEffPanelWidth() 按它的实际宽度设置日志页分隔条
    m_grpWaveInfo = grpWave;
    QGridLayout* waveLayout = new QGridLayout(grpWave);
    // ★ 2026-09-13 客户要求「波次面板字体放大一点」：13px → 15px（标签与数值同步）
    //   标签宽度也相应放宽，避免放大后文字被挤/换行
    static const char* kWaveLabelQss = "font-size: 15px; min-width: 116px;";
    static const char* kWaveValueQss = "font-size: 15px; font-weight: bold; color: #2196F3;";

    auto makeLabel = [](const QString& title) {
        QLabel* label = new QLabel(title);
        label->setStyleSheet(kWaveLabelQss);
        return label;
    };
    auto makeValue = []() {
        QLabel* label = new QLabel("--");
        label->setStyleSheet(kWaveValueQss);
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
    // ★ 2026-09-13 新增字段（客户需求）
    m_lblPlanQty     = makeValue();   // 计划件数（orderQty）
    m_lblExcBin      = makeValue();   // 异常口（= 仍在异常口、尚未处理完的件数，成功落格即递减）
    m_lblExcTrace    = makeValue();   // ★ 留痕条数（exception_record；标签文案「留痕」）
    m_lblRfidScanCount = makeValue(); // RFID 扫描次数（= RFID 推送 EPC 次数，重复计数）
    m_lblElapsed     = makeValue();   // 波次时长
    // ★ 2026-09-13 客户要求：波次面板不再显示"容器绑定"数值（改由第 0 页标签页展示）

    // ★ 2026-09-13 需求：「处理」右边加"显示按钮" —— 点击弹窗查看待处理/异常 EPC 的全部相关信息
    m_btnViewException = new QPushButton(QString::fromUtf8("查看处理"));
    m_btnViewException->setMinimumHeight(24);
    m_btnViewException->setCursor(Qt::PointingHandCursor);
    m_btnViewException->setStyleSheet(
        "QPushButton { font-size: 12px; font-weight: bold; padding: 1px 8px; border-radius: 3px;"
        " background-color: #D32F2F; color: white; border: none; }"
        "QPushButton:hover { background-color: #B71C1C; }"
        "QPushButton:disabled { background-color: #BDBDBD; }");
    m_btnViewException->setToolTip(QString::fromUtf8(
        "查看本波次待处理/异常明细（exception_record 留痕）：\n"
        "  · 处理 = 仍在异常口、尚未处理完的件数（去重 EPC）；该 EPC 之后成功落格即视为已处理，数量立即减少\n"
        "  · 异常口 = 与「处理」同一个量（同一批待处理件），成功落格时同步减少\n"
        "  · 留痕 = exception_record 记录条数（含未匹配/未绑定/冲突/重扫/超时等仅留痕项）\n"
        "双击弹窗内任一行可查看该 EPC 的全信息（分拣历史/异常历史/计划明细）"));
    connect(m_btnViewException, &QPushButton::clicked, this, &MainWindow::onViewExceptions);
    // 异常数值 + 按钮同一水平单元格（按钮紧跟数值右侧）
    QWidget* excCell = new QWidget();
    QHBoxLayout* excCellLayout = new QHBoxLayout(excCell);
    excCellLayout->setContentsMargins(0, 0, 0, 0);
    excCellLayout->setSpacing(6);
    excCellLayout->addWidget(m_lblException);
    excCellLayout->addWidget(m_btnViewException);
    excCellLayout->addStretch();

    // ★ 2026-09-13 留痕 + 查看按钮（同"处理"的交互；仅留痕项不计入处理数）
    QWidget* excTraceCell = new QWidget();
    QHBoxLayout* excTraceCellLayout = new QHBoxLayout(excTraceCell);
    excTraceCellLayout->setContentsMargins(0, 0, 0, 0);
    excTraceCellLayout->setSpacing(6);
    excTraceCellLayout->addWidget(m_lblExcTrace);
    excTraceCellLayout->addStretch();

    // ★ 2026-09-08 UI调整：两列成对排布（每行左/右各一组 标签+值）
    int row = 0;
    auto addFieldPair = [&](const QString& t1, QWidget* v1, const QString& t2, QWidget* v2) {
        waveLayout->addWidget(makeLabel(t1), row, 0);
        waveLayout->addWidget(v1,            row, 1);
        if (v2) {
            waveLayout->addWidget(makeLabel(t2), row, 2);
            waveLayout->addWidget(v2,            row, 3);
        }
        ++row;
    };
    // ★ 2026-09-13 字段扩充（客户需求：口径显式化 + 新增 RFID 扫描次数/异常口）
    //   ★ 客户口径变更：「异常」改称「处理」（= 仍在异常口、尚未处理完的件数）
    addFieldPair("波次号:",   m_lblWaveCode,     "状态:",     m_lblWaveStatus);
    addFieldPair("SKU数:",    m_lblSkuCount,     "计划:",     m_lblPlanQty);
    addFieldPair("已分拣:",   m_lblSorted,       "分拣件数:", m_lblSumLocation);
    addFieldPair("处理:",     excCell,           "异常口:",   m_lblExcBin);
    addFieldPair("留痕:",     excTraceCell,      "RFID扫描:", m_lblRfidScanCount);
    addFieldPair("效率:",     m_lblEfficiency,   "峰值效率:", m_lblPeakEff);
    addFieldPair("波次时长:", m_lblElapsed,      "上波次:",   m_lblLastWave);
    // ★ 2026-09-13 超计划预警：数字 + 「查看」按钮（点击查看落了几件/哪个格口容器/计划几件/多余几件）
    {
        QWidget* overplanCell = new QWidget();
        QHBoxLayout* overplanLayout = new QHBoxLayout(overplanCell);
        overplanLayout->setContentsMargins(0, 0, 0, 0);
        overplanLayout->setSpacing(6);

        m_lblOverplanWarn = new QLabel("0");
        m_lblOverplanWarn->setStyleSheet("font-size: 13px; font-weight: bold; color: #555;");
        m_lblOverplanWarn->setToolTip(QString::fromUtf8(
            "超计划预警条目数（按 格口+SKU 统计：实际落格件数 > 计划件数的条目）\n"
            "成因：同一 SKU 有两件同时在线上、或人工多放，导致箱内实落超过计划\n"
            "处置：点击右侧「查看」查看明细（落了几件 / 哪个格口哪个容器 / 计划几件 / 多余几件），"
            "多余件请现场从对应容器取出"));

        m_btnOverplanView = new QPushButton(QCoreApplication::translate("MainWindow", "查看"));
        m_btnOverplanView->setMinimumHeight(24);
        m_btnOverplanView->setStyleSheet(
            "QPushButton { font-size: 12px; padding: 2px 10px; "
            "background-color: #FFE0B2; color: #E65100; border: 1px solid #FFB74D; border-radius: 3px; } "
            "QPushButton:hover { background-color: #FFCC80; } "
            "QPushButton:disabled { background-color: #EEEEEE; color: #AAAAAA; border-color: #DDDDDD; }");
        m_btnOverplanView->setEnabled(false);
        connect(m_btnOverplanView, &QPushButton::clicked, this, &MainWindow::showOverplanWarningDialog);

        overplanLayout->addWidget(m_lblOverplanWarn);
        overplanLayout->addWidget(m_btnOverplanView);
        overplanLayout->addStretch();
        addFieldPair("预警:", overplanCell, QString(), nullptr);
    }
    // ★ 2026-09-13 客户要求：**波次信息面板不再显示容器绑定数据**
    //   （容器绑定状态在第 0 页标签页完整展示；此处仅保留波次自身字段）
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
    // ★ 2026-09-13 标签页 ①：容器绑定状态面板（66格口，4列×17行网格）
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
    // ★ 2026-09-16 需求⑦：图例改为"整格底色口径"（色名按实际颜色着色，与面板一一对应）
    //   ★ 本次更新：绿色改为 #26A96C；**未绑定不设颜色**（无色=默认状态）→ 图例不再给它色块
    QLabel* bindHint = new QLabel(QString::fromUtf8(
        "<span style='color:%1;'>■</span> 绿色=已绑定&nbsp;&nbsp;"
        "<span style='color:%2;'>■</span> 橙色=满箱锁格&nbsp;&nbsp;"
        "<span style='color:%3;'>■</span> 红色=已解锁·待重绑&nbsp;&nbsp;"
        "未绑定=无底色（默认）")
        .arg(BP_COLOR_BOUND).arg(BP_COLOR_LOCKED).arg(BP_COLOR_REBIND));
    bindHint->setStyleSheet("font-size: 12px; color: #555;");
    bindHint->setToolTip(QString::fromUtf8(
        "整格底色 = 该格口的容器绑定状态（同格口多状态同时成立时优先级：锁格 > 待重绑 > 已绑定 > 未绑定）\n"
        "绿=已绑定容器；橙=满箱锁格（PLC 锁格位置位）；红=已物理解锁但仍禁用（等 WMS 重发 H6）；\n"
        "未绑定 = **不设颜色**（面板默认底色，深灰字）\n"
        "右侧「异常口：N格口」= 配置的物理异常口（exceptionGrid）：不在此面板展示（不显示格子、不计入计数），\n"
        "该口只收超计划件、需人工清出、不上传 WMS。"));
    bindBtnRow->addWidget(btnRefreshBind);
    bindBtnRow->addWidget(btnClearBinds);
    bindBtnRow->addWidget(bindHint);

    // ★ 2026-09-17 现场要求：面板上明文标出被隐藏的异常口（表述：异常口：66格口）
    //   格号取自配置 exceptionGrid（兼容 "66"/"066"/"22066" 写法，统一显示为十进制格口号）；
    //   未配置（空 / "0"）→ 显示"异常口：未配置"（此时面板不隐藏任何格口）。
    m_lblExceptionGrid = new QLabel();
    {
        const QString excCfg  = ConfigManager::instance()->config().exceptionGrid.trimmed();
        const bool    excNone = (excCfg.isEmpty() || excCfg == "0");
        const int     excNum  = excNone ? -1 : parseWmsGridCodeToInt(excCfg);
        if (excNum > 0)
        {
            m_lblExceptionGrid->setText(QString::fromUtf8("异常口：%1格口").arg(excNum));
            m_lblExceptionGrid->setToolTip(QString::fromUtf8(
                "配置的物理异常口 = %1 号格口（<exceptionGrid>%2</exceptionGrid>）。\n"
                "该口**不在本面板展示**：不显示格子、不计入「已绑定/已锁格/未绑定」计数。\n"
                "它的用途：承接超计划件，需人工清出；该口的落格件不上传 WMS（不进 H7）。")
                .arg(excNum).arg(excCfg));
        }
        else
        {
            m_lblExceptionGrid->setText(QString::fromUtf8("异常口：未配置"));
            m_lblExceptionGrid->setToolTip(QString::fromUtf8(
                "当前未配置物理异常口（<exceptionGrid> 为空或 0）→ 面板**不隐藏任何格口**（66 格全部展示）。"));
        }
        m_lblExceptionGrid->setStyleSheet("font-size: 12px; font-weight: bold; color: #555;");
    }
    bindBtnRow->addWidget(m_lblExceptionGrid);
    // ★ 2026-09-17 绑定"落库失败"红字（默认隐藏）：此前失败只写 data.log，
    //   现场表现为"面板显示已绑定、库里一行都没有"→ 切回该波次无绑定可恢复。
    m_lblBindPersistFailed = new QLabel();
    m_lblBindPersistFailed->setStyleSheet("font-size: 13px; font-weight: bold; color: #D32F2F;");
    m_lblBindPersistFailed->setVisible(false);
    bindBtnRow->addWidget(m_lblBindPersistFailed);
    bindBtnRow->addStretch();

    // 已绑定/已锁格/未绑定计数（★ 2026-09-16：口径与面板可见格口数一致，不含被隐藏的异常口）
    m_lblBoundCount = new QLabel();
    m_lblLockedCount = new QLabel();   // ★ 2026-09-11：已锁格数量
    m_lblUnboundCount = new QLabel();
    m_lblBoundCount->setStyleSheet(
        QString("font-size: 13px; font-weight: bold; color: %1; padding: 0 8px;").arg(BP_COLOR_BOUND));
    // ★ 底色与"锁格"格口同色系：底同色 + 白字（小字号可读）
    m_lblLockedCount->setStyleSheet(
        QString("font-size: 13px; font-weight: bold; color: #FFFFFF; background-color: %1;"
                " border-radius: 7px; padding: 1px 8px;").arg(BP_COLOR_LOCKED));
    m_lblUnboundCount->setStyleSheet(
        QString("font-size: 13px; font-weight: bold; color: %1; padding: 0 8px;").arg(BP_COLOR_REBIND));
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

    // ★ 2026-09-16 需求⑦：外框样式统一由 tests/BindingPanelPolicy.h::bpFrameStyle() 给出
    //   （有色态=底色+边框；未绑定=无色，只留边框 → 面板默认底色透出）
    //   建格时即"未绑定"态 —— 开机为新任务状态（需求④），颜色与状态天然一致

    // 创建格口绑定标签（6 列；★ 需求②：异常口整格不渲染，可见格按"可见序号"重排位置不留空洞）
    int visIdx = 0;
    for (int i = 0; i < BINDING_SLOT_COUNT; ++i)
    {
        const int gridNum = i + 1;
        if (isGridHiddenInBindingPanel(gridNum)) continue;   // ★ 需求②：异常口不渲染

        const int col = visIdx % m_bindingCols;
        const int row = visIdx / m_bindingCols;
        ++visIdx;

        // 每个格口一个 Frame 包裹（★ 需求⑦：整格底色 = 绑定状态；未绑定=无色）
        QFrame* frame = new QFrame();
        frame->setFrameShape(QFrame::Box);
        const QString initStyle = bpFrameStyle(BP_UNBOUND);   // ★ 未绑定：不设背景（无色）
        frame->setStyleSheet(initStyle);
        m_bindingFrames.append(frame);
        m_bindingInitialStyles.append(initStyle);
        frame->setMinimumHeight(36);

        QHBoxLayout* fLayout = new QHBoxLayout(frame);
        fLayout->setContentsMargins(2, 1, 2, 1);
        fLayout->setSpacing(1);

        // ★ 格口标签：优先使用 XML 自定义名，否则零填充序号
        //   ★ 字号调大：11px → 13px；颜色在刷新时按状态切换（有色底=白字 / 无色底=深灰字）
        QString paddedNum = QString("%1").arg(gridNum, GRID_KEY_PADDING, 10, QChar('0'));
        QString displayName = ConfigManager::instance()->config().gridNames.value(QString::number(gridNum), paddedNum);
        QLabel* lblGrid = new QLabel(displayName);
        lblGrid->setFixedWidth(80);
        lblGrid->setAlignment(Qt::AlignCenter);
        lblGrid->setStyleSheet("font-size: 13px; font-weight: bold; color: #333333; border: none; background: transparent;");

        // ★ 2026-09-16 需求⑦：状态圆点去掉（整格底色已表达状态）——对象保留但隐藏，
        //   这样 updateBindingPanel 内对它的赋值仍然安全（无需两处循环一起改结构）
        QLabel* lblStatus = new QLabel("--");
        lblStatus->setFixedWidth(12);
        lblStatus->setFixedHeight(12);
        lblStatus->setAlignment(Qt::AlignCenter);
        lblStatus->setStyleSheet(
            "font-size: 10px; color: white; border-radius: 6px; background-color: transparent;");
        lblStatus->setToolTip(QString("格口%1: 未绑定").arg(
            QString("%1").arg(gridNum, GRID_KEY_PADDING, 10, QChar('0'))));
        lblStatus->setVisible(false);

        // ★ 字号调大：11px → 13px（初始为"未绑定"态 → 深灰字）
        QLabel* lblBox = new QLabel("--");
        lblBox->setStyleSheet(QString("font-size: 13px; color: %1; font-weight: bold;"
                                      " border: none; background: transparent;")
                                  .arg(BP_TEXT_COLOR_PLAIN));
        lblBox->setMinimumWidth(90);

        fLayout->addWidget(lblGrid);
        fLayout->addWidget(lblStatus);
        fLayout->addWidget(lblBox);

        m_bindingGrid->addWidget(frame, row, col);

        // ★ 存储标签指针（后续刷新直接索引，避免 findChildren 递归查找）
        m_bindingLabels[i]    = lblStatus;
        m_bindingBoxLabels[i] = lblBox;
    }
    m_bindingRows = (visIdx + m_bindingCols - 1) / m_bindingCols;   // ★ 按可见格口数计算行数

    scrollBinding->setWidget(m_bindingWidget);
    bindOuterLayout->addLayout(bindBtnRow);
    bindOuterLayout->addWidget(scrollBinding);

    // ═══════════════════════════════════════════
    // ★ 2026-09-13 标签页 ③：波次数据历史记录（原「波次数据记录（全部已传输波次）」）
    //   刷新（手动） | 切换选中波次（恢复进度继续；★ 2026-09-16 终态波次拒绝切回） | 新任务（保留当前波次进度，清空待接收）
    //   H7/H8 重传按钮位于「任务接收控制」区（主工作流保障，不随本面板操作）
    // ═══════════════════════════════════════════
    QGroupBox* grpUnfinished = new QGroupBox(QCoreApplication::translate("MainWindow", "波次数据历史记录（全部已传输波次）"));
    QVBoxLayout* unfinishedLayout = new QVBoxLayout(grpUnfinished);

    QHBoxLayout* unfinishedBtnRow = new QHBoxLayout();
    m_btnRefreshWaves = new QPushButton(QCoreApplication::translate("MainWindow", "刷新"));
    m_btnResumeWave   = new QPushButton(QCoreApplication::translate("MainWindow", "切换选中波次"));
    // ★ 2026-09-17 新增：只读查看选中波次「每格最后绑定的容器」
    //   现场问题"该波次下的格口绑定状态与显示不会随切回回溯"→ 给出不依赖切回的只读入口，
    //   切回前即可核对"这个波次当时绑的是哪些箱子"，也是历史数据留存的可视化核对手段。
    m_btnViewWaveBinds = new QPushButton(QCoreApplication::translate("MainWindow", "查看绑定"));
    m_btnRefreshWaves->setMinimumHeight(34);
    m_btnResumeWave->setMinimumHeight(34);
    m_btnViewWaveBinds->setMinimumHeight(34);
    // ★ 2026-09-13 需求：本页控件/字体整体放大（与"分拣记录查询"页统一）
    const QFont bigBtnFont(font().family(), 13);
    m_btnRefreshWaves->setFont(bigBtnFont);
    m_btnResumeWave->setFont(bigBtnFont);
    m_btnViewWaveBinds->setFont(bigBtnFont);
    m_btnRefreshWaves->setStyleSheet(
        "QPushButton { background-color: #2196F3; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #1976D2; }");
    m_btnResumeWave->setStyleSheet(
        "QPushButton { background-color: #4CAF50; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #388E3C; }");
    m_btnViewWaveBinds->setStyleSheet(
        "QPushButton { background-color: #607D8B; color: white; font-size: 13px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #455A64; }");
    // ★ 2026-09-17 现场要求：「查看绑定」按钮**隐藏**（列表里不再出现该入口）。
    //   实现为"隐藏"而非删代码：只读弹窗（onViewWaveBinds）与取数口径全部保留，
    //   恢复显示只需把下面这行的 false 改成 true。
    //   注：隐藏的控件不占布局空间，按钮行仍为 刷新 / 切换选中波次。
    //   该波次的格口绑定仍可在数据库侧只读核查：python docs/wave_bind_audit.py
    m_btnViewWaveBinds->setVisible(false);
    // ★ 2026-09-17 现场要求：「新任务」按钮**移到「设置配置」旁边**（任务接收控制区同一行）。
    //   此处不再创建/放置该按钮，见下方任务接收控制区 viewQueueRow；按钮的 clicked 连接保持不变
    //   （仍在 setupCore 之后的统一 connect 段里连到 onStartNewWaveTask）。
    unfinishedBtnRow->addWidget(m_btnRefreshWaves);
    unfinishedBtnRow->addWidget(m_btnResumeWave);
    unfinishedBtnRow->addWidget(m_btnViewWaveBinds);
    unfinishedBtnRow->addStretch();

    m_tblWaveRecords = new QTableWidget();
    // ★ 2026-09-17 第 10 列「格口绑定」（追加在末尾 → 既有列索引 0~8 全部不变，
    //   onResumeSelectedWave 读列 0、终态标注读列 1 均不受影响）。
    //   ★★ 现场要求：本列**默认隐藏**（列表里不再显示）★★ —— 见下方 setColumnHidden。
    //   列头、取数口径与「查看绑定」弹窗全部保留：恢复显示只需把那一行的 true 改成 false。
    m_tblWaveRecords->setColumnCount(WAVE_RECORDS_COLUMN_COUNT);
    // ★ 2026-09-13 列头显式化口径（客户口径：「处理」= 仍在异常口待处理件数；异常口同值）
    m_tblWaveRecords->setHorizontalHeaderLabels(
        QStringList() << "波次号" << "状态" << "计划件数" << "已分拣(件次)" << "处理(件)"
                      << "异常口(件)" << "H7满箱" << "H8完结" << "更新时间" << "格口绑定");
    // ★ 2026-09-17 现场要求：隐藏「格口绑定」列（做成"可一键恢复"的隐藏，而不是删代码）
    m_tblWaveRecords->setColumnHidden(WAVE_RECORDS_BIND_COL, true);
    m_tblWaveRecords->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tblWaveRecords->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tblWaveRecords->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tblWaveRecords->horizontalHeader()->setStretchLastSection(true);
    m_tblWaveRecords->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tblWaveRecords->setMinimumHeight(200);
    m_tblWaveRecords->setFont(QFont(font().family(), 13));   // ★ 2026-09-13 字体放大
    m_tblWaveRecords->verticalHeader()->setDefaultSectionSize(28);
    m_tblWaveRecords->setStyleSheet(
        "QTableWidget { font-size: 13px; }"
        "QTableWidget::item { padding: 3px 6px; }"
        "QHeaderView::section { background-color: #e0e0e0; font-weight: bold; padding: 6px; font-size: 13px; }");
    m_tblWaveRecords->setToolTip(QString::fromUtf8(
        "口径说明：\n"
        "  已分拣(件次) = PLC 落格反馈累计件次（含重复反馈与重投）\n"
        "  处理(件)     = 仍在异常口、尚未处理完的件数（去重 EPC）——该 EPC 之后成功落格即视为已处理，立即减少\n"
        "  异常口(件)   = 与「处理」同一个量（同源同值），成功落格时同步减少\n"
        "  当前波次取内存实时值；历史波次取数据库未闭环计数\n"
        "  需要「本波次曾掉入异常口的总量」请看波次面板的「异常留痕」或异常明细弹窗\n"
        "  格口绑定：本列表与界面**不显示**该列/入口（现场要求）；需要核对该波次绑了哪些容器时，\n"
        "            用只读脚本 docs/wave_bind_audit.py（只读、不改数据）\n"
        "双击行 = 切换选中波次"));
    // ★ 双击行 = 切换选中波次
    connect(m_tblWaveRecords, &QTableWidget::cellDoubleClicked, this,
        [this](int, int) { onResumeSelectedWave(); });

    unfinishedLayout->addLayout(unfinishedBtnRow);
    unfinishedLayout->addWidget(m_tblWaveRecords);

    connect(m_btnRefreshWaves, &QPushButton::clicked, this, &MainWindow::onRefreshWaveRecords);
    connect(m_btnResumeWave,   &QPushButton::clicked, this, &MainWindow::onResumeSelectedWave);
    connect(m_btnViewWaveBinds, &QPushButton::clicked, this, &MainWindow::onViewWaveBinds);
    connect(m_btnNewTask,      &QPushButton::clicked, this, &MainWindow::onStartNewWaveTask);

    // ★ 2026-09-17 只读查看绑定：不依赖切回、不受终态限制（数据仍在库里，只读回溯需求）
    if (m_btnViewWaveBinds)
    {
        m_btnViewWaveBinds->setToolTip(QCoreApplication::translate("MainWindow",
            "只读查看选中波次的格口绑定（每格取该波次内最后一条绑定 = 切回时恢复的口径）。\n"
            "不切换波次、不改任何数据；「无绑定记录」表示切回时恢复不出该波次的绑定。"));
    }

    // ★ 2026-09-16 现场需求①：选中终态行（已完成/已取消）→ 禁用「切换选中波次」并给出正确入口提示
    if (m_btnResumeWave)
    {
        m_btnResumeWave->setToolTip(QCoreApplication::translate("MainWindow",
            "切换到选中波次并按其上次进度继续（进度、明细、格口绑定一并恢复）"));
        connect(m_tblWaveRecords, &QTableWidget::itemSelectionChanged, this, [this]() {
            if (!m_btnResumeWave) return;
            bool bTerminal = false;
            const int r = m_tblWaveRecords ? m_tblWaveRecords->currentRow() : -1;
            if (r >= 0 && m_tblWaveRecords->item(r, 1))
            {
                const QString st = m_tblWaveRecords->item(r, 1)->text();
                bTerminal = st.contains(QString::fromUtf8("（不可切回）"));
            }
            m_btnResumeWave->setEnabled(!bTerminal);
            m_btnResumeWave->setToolTip(bTerminal
                ? QCoreApplication::translate("MainWindow",
                    "该波次已完成/已取消：按需求**不允许切回**。\n"
                    "如需补发失败报文，请点「重传满箱切换(H7)」或「重传任务完结(H8)」")
                : QCoreApplication::translate("MainWindow",
                    "切换到选中波次并按其上次进度继续（进度、明细、格口绑定一并恢复）"));
        });
    }

    // ═══════════════════════════════════════════
    // ★ 2026-09-13 标签页 ②：分拣记录查询（客户要求"字体/控件大小调大一些"）
    // ═══════════════════════════════════════════
    QGroupBox* grpQuery = new QGroupBox(QCoreApplication::translate("MainWindow", "分拣记录查询"));
    QVBoxLayout* queryLayout = new QVBoxLayout(grpQuery);

    // ★ 2026-09-13 本页统一放大参数（控件字体 & 行高）
    const QFont queryFont(font().family(), 13);
    const int   queryCtlH = 36;

    // ── 查询条件行 ──
    QHBoxLayout* queryCondRow = new QHBoxLayout();
    queryCondRow->setSpacing(8);

    // ★ 查询模式下拉框
    queryCondRow->addWidget(new QLabel(QCoreApplication::translate("MainWindow", "查询模式:")));
    m_cmbQueryMode = new QComboBox();
    m_cmbQueryMode->addItem(QCoreApplication::translate("MainWindow", "按EPC查询"));
    m_cmbQueryMode->addItem(QCoreApplication::translate("MainWindow", "按SKU查询格口"));
    m_cmbQueryMode->addItem(QCoreApplication::translate("MainWindow", "按格口查询"));   // ★ 2026-09-09 需求2
    m_cmbQueryMode->addItem(QCoreApplication::translate("MainWindow", "按容器号查询")); // ★ 2026-09-13 需求：查该容器下所有EPC物件
    m_cmbQueryMode->setMinimumWidth(160);
    m_cmbQueryMode->setMinimumHeight(queryCtlH);   // ★ 2026-09-13 控件放大
    m_cmbQueryMode->setFont(queryFont);
    queryCondRow->addWidget(m_cmbQueryMode);

    // ★ EPC编码输入（默认显示）
    m_editQueryBarcode = new QLineEdit();
    m_editQueryBarcode->setPlaceholderText(QCoreApplication::translate("MainWindow", "输入EPC编码查询（留空查全部）"));
    m_editQueryBarcode->setMinimumWidth(220);
    m_editQueryBarcode->setMinimumHeight(queryCtlH);
    m_editQueryBarcode->setFont(queryFont);
    queryCondRow->addWidget(m_editQueryBarcode);

    // ★ SKU编码输入（默认隐藏，按SKU查询时显示）
    m_editQuerySku = new QLineEdit();
    m_editQuerySku->setPlaceholderText(QCoreApplication::translate("MainWindow", "输入SKU编码（显示分配格口 + 每个EPC的实际落格号）"));
    m_editQuerySku->setMinimumWidth(220);
    m_editQuerySku->setMinimumHeight(queryCtlH);
    m_editQuerySku->setFont(queryFont);
    m_editQuerySku->setVisible(false);
    queryCondRow->addWidget(m_editQuerySku);

    queryCondRow->addWidget(new QLabel(QCoreApplication::translate("MainWindow", "日期:")));
    m_editQueryDateFrom = new QDateEdit(QDate::currentDate().addDays(-7));
    m_editQueryDateFrom->setCalendarPopup(true);
    m_editQueryDateFrom->setDisplayFormat("yyyy-MM-dd");
    m_editQueryDateFrom->setMinimumHeight(queryCtlH);
    m_editQueryDateFrom->setFont(queryFont);
    queryCondRow->addWidget(m_editQueryDateFrom);

    queryCondRow->addWidget(new QLabel("~"));
    m_editQueryDateTo = new QDateEdit(QDate::currentDate());
    m_editQueryDateTo->setCalendarPopup(true);
    m_editQueryDateTo->setDisplayFormat("yyyy-MM-dd");
    m_editQueryDateTo->setMinimumHeight(queryCtlH);
    m_editQueryDateTo->setFont(queryFont);
    queryCondRow->addWidget(m_editQueryDateTo);

    // ★ 2026-09-16 现场需求③：日期区间是**必填条件**（没有"全部/不筛日期"选项）——
    //   四种查询模式（按EPC/按SKU/按格口/按容器号）的结果都只包含区间内的落格记录；
    //   「待分拣」计划行没有落格时间，不受日期筛选影响（口径见 tooltip）。
    {
        const QString dateTip = QString::fromUtf8(
            "日期区间（必填，作用于落格时间 sort_time）：\n"
            "  · 起 = 该日 00:00:00，止 = 该日 23:59:59（含首含尾）\n"
            "  · 四种查询模式（按EPC/按SKU/按格口/按容器号）都只返回区间内的结果\n"
            "  · 「待分拣」计划明细行没有落格时间，**不受日期筛选影响**，照常显示\n"
            "  · 默认查最近 7 天；需要更早数据请自行调整起始日期");
        m_editQueryDateFrom->setToolTip(dateTip);
        m_editQueryDateTo->setToolTip(dateTip);
    }

    m_btnQueryRecords = new QPushButton(QCoreApplication::translate("MainWindow", "查询"));
    m_btnQueryRecords->setMinimumHeight(queryCtlH);
    m_btnQueryRecords->setMinimumWidth(84);
    m_btnQueryRecords->setFont(queryFont);
    m_btnQueryRecords->setStyleSheet(
        "QPushButton { background-color: #2196F3; color: white; font-size: 14px; font-weight: bold; "
        "border-radius: 4px; padding: 6px 18px; }"
        "QPushButton:hover { background-color: #1976D2; }");
    queryCondRow->addWidget(m_btnQueryRecords);

    m_btnQueryClear = new QPushButton(QCoreApplication::translate("MainWindow", "清空"));
    m_btnQueryClear->setMinimumHeight(queryCtlH);
    m_btnQueryClear->setMinimumWidth(72);
    m_btnQueryClear->setFont(queryFont);
    queryCondRow->addWidget(m_btnQueryClear);

    // ★ 2026-09-16 需求③：「效率统计」按钮已移出本页 → 挪到「任务接收控制」区，
    //   与「查看接收波次队列」「设置配置」同一水平行、位于「设置配置」左侧（见 serverLayout）
    queryCondRow->addStretch();

    // ── 统计标签 ──
    QHBoxLayout* statsRow = new QHBoxLayout();
    m_lblRecordCount = new QLabel(QCoreApplication::translate("MainWindow", "共 0 条记录"));
    m_lblRecordCount->setStyleSheet("font-size: 14px; color: #555; font-weight: bold;");
    m_lblDbStats = new QLabel("");
    m_lblDbStats->setStyleSheet("font-size: 14px; color: #2196F3;");
    statsRow->addWidget(m_lblRecordCount);
    statsRow->addWidget(m_lblDbStats);
    statsRow->addStretch();

    // ── 结果表格 ──（★ 2026-09-13 字体放大：单元格 14px / 表头 14px / 行高 32）
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
    m_tblRecords->setMinimumHeight(200);
    // ★ 2026-09-13：取消原 300px 高度上限——本页已是满高标签页，表格应吃满剩余空间
    m_tblRecords->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tblRecords->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tblRecords->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tblRecords->setAlternatingRowColors(true);
    m_tblRecords->horizontalHeader()->setStretchLastSection(true);
    m_tblRecords->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tblRecords->verticalHeader()->setVisible(false);
    m_tblRecords->verticalHeader()->setDefaultSectionSize(32);   // ★ 行高放大
    m_tblRecords->setFont(QFont(font().family(), 14));
    m_tblRecords->setStyleSheet(
        "QTableWidget { font-size: 14px; }"
        "QTableWidget::item { padding: 5px 8px; }"
        "QHeaderView::section { background-color: #e0e0e0; font-weight: bold; padding: 7px; font-size: 14px; }");
    m_tblRecords->setToolTip(QString::fromUtf8("双击任意一行可查看该 EPC 的全信息（分拣历史/异常历史/计划明细）"));

    queryLayout->addLayout(queryCondRow);
    queryLayout->addLayout(statsRow);
    queryLayout->addWidget(m_tblRecords, 1);

    // 连接信号
    connect(m_btnQueryRecords, &QPushButton::clicked, this, &MainWindow::onQueryRecords);
    connect(m_btnQueryClear,   &QPushButton::clicked, this, [this]() {
        m_tblRecords->setRowCount(0);
        m_lblRecordCount->setText(QCoreApplication::translate("MainWindow", "共 0 条记录"));
        m_lblDbStats->setText("");
    });
    // ★ 2026-09-16 需求③：本页不再有「效率统计」按钮（已移到「任务接收控制」区），
    //   故此处不再连接 onOpenEffChart；该按钮的显隐逻辑见任务接收控制区
    // 回车触发查询
    connect(m_editQueryBarcode, &QLineEdit::returnPressed, this, &MainWindow::onQueryRecords);
    connect(m_editQuerySku,     &QLineEdit::returnPressed, this, &MainWindow::onQueryRecords);

    // ★ 2026-09-13 需求：双击查询结果行 → 该 EPC 全信息窗（占位"待分拣"行无 EPC 时不弹）
    connect(m_tblRecords, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        for (int c = 0; c < m_tblRecords->columnCount(); ++c)
        {
            QTableWidgetItem* it = m_tblRecords->item(row, c);
            if (!it) continue;
            const QString h = m_tblRecords->horizontalHeaderItem(c)
                ? m_tblRecords->horizontalHeaderItem(c)->text() : QString();
            if (h == QString::fromUtf8("EPC编码") || h == QString::fromUtf8("EPC"))
            {
                const QString epc = it->text().trimmed();
                if (!epc.isEmpty() && epc != "--" && epc != QString::fromUtf8("—"))
                    showEpcDetail(epc);
                return;
            }
        }
    });

    // ★ 查询模式切换：显示/隐藏对应输入框
    //   0=按EPC  1=按SKU（用 SKU 输入框）  2=按格口  3=按容器号（2/3 复用 EPC 输入框）
    connect(m_cmbQueryMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        bool isSkuMode = (index == 1);
        m_editQueryBarcode->setVisible(!isSkuMode);
        m_editQuerySku->setVisible(isSkuMode);
        if (index == 2)
            m_editQueryBarcode->setPlaceholderText(QString::fromUtf8("输入格口号查询该格明细（7 / 007 / 22007 均可；留空查全格口汇总）"));
        else if (index == 3)
            m_editQueryBarcode->setPlaceholderText(QString::fromUtf8("输入容器号查询该容器下全部EPC物件（如 H-T0129；留空列出全部容器汇总）"));
        else
            m_editQueryBarcode->setPlaceholderText(QString::fromUtf8("输入EPC编码查询（留空查全部）"));
    });

    // ═══════════════════════════════════════════
    // ★ 2026-09-13 标签页 ⑤：运行日志
    //   ★ 2026-09-16 需求④：右侧可显示「RFID 推送效率统计（当日观察）」面板，
    //     由「任务接收控制」区的「效率统计」按钮控制显隐（默认开启）。
    //     · 面板**按需懒创建**：首次显示时才 new（此时 setupCore() 已建好 HttpServer，
    //       不会出现"面板已建但 m_pServer 还是空指针"的瞬间）；
    //     · 布局 = 水平分隔条：左=日志+清空按钮（吃满余量），右=效率面板（约 340px）；
    //     · 面板不可见时（按钮弹起 / 切到别的标签页）不刷新，不影响分拣主流程。
    // ═══════════════════════════════════════════
    QGroupBox* grpLog = new QGroupBox("运行日志");
    QVBoxLayout* logLayout = new QVBoxLayout(grpLog);

    m_txtLog = new QTextEdit();
    m_txtLog->setReadOnly(true);
    m_txtLog->document()->setMaximumBlockCount(LOG_MAX_BLOCK_COUNT);
    m_txtLog->setStyleSheet("font-family: Consolas, 'Microsoft YaHei'; font-size: 12px;");

    QPushButton* btnClearLog = new QPushButton("清空日志");
    btnClearLog->setMinimumHeight(34);
    btnClearLog->setFont(QFont(font().family(), 13));

    {
        QWidget* logLeft = new QWidget();
        QVBoxLayout* logLeftLay = new QVBoxLayout(logLeft);
        logLeftLay->setContentsMargins(0, 0, 0, 0);
        logLeftLay->addWidget(m_txtLog, 1);
        logLeftLay->addWidget(btnClearLog);

        // 右侧容器：面板懒创建后放进来（未创建时容器为空、不占宽度）
        m_logEffHost = new QWidget(grpLog);
        {
            QVBoxLayout* hostLay = new QVBoxLayout(m_logEffHost);
            hostLay->setContentsMargins(0, 0, 0, 0);
        }
        m_logEffHost->setVisible(false);   // 默认不占位（面板首次 tick 后由按钮状态决定）

        m_logEffSplit = new QSplitter(Qt::Horizontal, grpLog);
        m_logEffSplit->setChildrenCollapsible(false);
        m_logEffSplit->setHandleWidth(5);
        m_logEffSplit->addWidget(logLeft);
        m_logEffSplit->addWidget(m_logEffHost);
        // ★ 2026-09-17 现场要求：日志区 与 效率统计面板 **各占一半**
        //   （原为 stretch 1/0：日志吃余量、面板固定约 340px）
        //   两个 stretch 都置 1 → 初始尺寸一半一半（见 applyLogEffPanelVisible），
        //   窗口缩放时两者按同比例伸缩；现场仍可拖动中间分隔条自行调整。
        m_logEffSplit->setStretchFactor(0, 1);
        m_logEffSplit->setStretchFactor(1, 1);
        logLayout->addWidget(m_logEffSplit);
    }
    connect(btnClearLog, &QPushButton::clicked, this, &MainWindow::onClearLog);

    // ═══════════════════════════════════════════
    // ★ 2026-09-13 UI改版 —— 第二行：左侧标签页多页窗口的第 4 页「实时面板」
    //   = 合并后的「落格反馈数据（实时）」：序号｜时间｜EPC｜对应SKU｜格口号｜容器号｜小车号｜状态
    //   数据源：RFID 推送（RfidPushClient::rfidPushReceived，QueuedConnection 回主线程）
    //           + PLC 落格反馈（PlcManager::plcFeedbackBatch，100ms 批量信号）
    //   合并规则：RFID 先到 → 建一行"待落格"占位；PLC 反馈到达 → 就地补全同一行
    //   新行插第0行（最新在最上），超 LIVE_TABLE_MAX_ROWS 行自动裁掉最旧行
    // ═══════════════════════════════════════════
    // ★★ 2026-09-17 现场要求：本页可由 XML 开关 <showLivePage> 隐藏，且**改了不立刻生效**
    //    （只在启动时读一次配置）。关闭时**整张表都不创建**（m_tblLive 保持 nullptr）：
    //    实时面板的全部更新路径都以 `if (!m_tblLive) return;` 为第一道守卫
    //    → 隐藏期间 RFID/PLC 高频路径零 UI 开销（比"创建后 setVisible(false)"更省）。
    //    恢复显示：把 config/http_server.xml 的 <showLivePage> 改成 true 并**重启程序**。
    const bool showLivePage = ConfigManager::instance()->config().showLivePage;
    QGroupBox* grpLive = nullptr;
    if (showLivePage)
    {
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

    // ── 落格反馈数据（实时）表：序号｜时间｜EPC｜对应SKU｜格口号｜容器号｜小车号｜状态 ──
    grpLive = new QGroupBox(QString::fromUtf8("落格反馈数据（实时）"));
    QVBoxLayout* liveLayout = new QVBoxLayout(grpLive);
    liveLayout->setContentsMargins(6, 4, 6, 4);
    m_tblLive = new QTableWidget();
    styleLiveTable(m_tblLive, QStringList()
        << "序号" << "时间" << "EPC" << "对应SKU" << "格口号" << "容器号" << "小车号" << "状态");
    m_tblLive->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_tblLive->setColumnWidth(0, 64);
    m_tblLive->setColumnWidth(1, 92);
    m_tblLive->setColumnWidth(2, 168);
    m_tblLive->setColumnWidth(3, 128);
    m_tblLive->setColumnWidth(4, 84);
    m_tblLive->setColumnWidth(5, 130);
    m_tblLive->setColumnWidth(6, 150);
    m_tblLive->setColumnWidth(7, 168);
    m_tblLive->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);  // EPC列吃余量
    m_tblLive->setToolTip(QString::fromUtf8(
        "RFID 推送先占一行「待落格」，PLC 落格反馈到达后就地补全同一行。\n"
        "数据源：RFID推送（EPC/小车号/对应SKU）+ PLC落格反馈（格口号/容器号/首尾车/状态）。\n"
        "状态：1 成功 ｜ 2 无格口 ｜ 3 信息不全 ｜ 0（3字段格式无状态）；重投件标注（重投k次）。"));
    liveLayout->addWidget(m_tblLive);
    }
    else
    {
        // 开关关闭：不建表 → 后面所有实时面板更新路径自动跳过（零开销）
        m_tblLive = nullptr;
    }

    // ═══════════════════════════════════════════
    // ★ 2026-09-13 UI改版 组装布局：
    //   第一行：任务接收控制 ｜ 设备状态(PLC/RFID) ｜ 波次信息（保持不变）
    //   第二行：QTabWidget（标签在左侧），5 页——
    //     ① 容器绑定状态 ② 分拣记录查询 ③ 波次数据历史记录 ④ 实时面板 ⑤ 运行日志
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

    // ═══════════════════════════════════════════
    // 第二行：★ 2026-09-13 左侧标签页多页窗口（5 页，页序即客户要求的顺序）
    //   ① 容器绑定状态  ② 分拣记录查询  ③ 波次数据历史记录  ④ 实时面板  ⑤ 运行日志
    //   标签置于左侧（QTabWidget::West），标签宽度随文字（不拉伸占满整列）
    //   ★ 2026-09-13 修正：Qt 默认会把侧边标签文字整体旋转 90°，中文"拧着"不可读；
    //     改用 VerticalTabBar 自绘——中文按"逐字竖排"（每个字一行）显示，不旋转。
    // ═══════════════════════════════════════════
    m_tabMain = new VerticalTabWidget();   // ★ 自带"中文逐字竖排"自绘标签栏（见 VerticalTabBar.h）
    m_tabMain->setTabPosition(QTabWidget::West);
    m_tabMain->tabBar()->setExpanding(false);   // 标签宽度随文字，避免占满左侧整列
    // ★ 2026-09-13 客户要求：页切换选项文字更醒目、各项之间区分更明显
    //   要点（配合 VerticalTabBar 自绘，见该文件顶部"外观常量"）：
    //     · 字号 15 加粗；标签之间加分隔间距 + 更清晰的边框；
    //     · 选中项：蓝底白字 + 左侧强调条 + 右侧接页面的高亮边，未选中：浅灰底深灰字；
    //       —— 底色/字色/强调条差异让"当前在哪一页"一眼可辨（原来只有文字颜色深浅差别）
    {
        QFont tabFont = m_tabMain->tabBar()->font();
        tabFont.setPointSize(15);
        tabFont.setBold(true);
        m_tabMain->tabBar()->setFont(tabFont);
    }
    m_tabMain->setDocumentMode(true);
    // 标签栏控件自身样式（背景/边框由自绘接管，这里主要给 pane 与内边距）
    m_tabMain->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #B0BEC5; background: #FFFFFF; }"
        "QTabBar { background: #ECEFF1; }"
        // ★ 2026-09-17 现场要求：左侧页签按钮栏**加宽一些**（更好按、更好看）
        //   宽度口径 = max(min-width, 文字高) + 左右内边距 + 边框；实测（offscreen 探针）：
        //     旧 26px + padding 6px  → 单页签 40px
        //     新 34px + padding 10px → 单页签 56px（+40%）
        //   文字仍逐字竖排居中（VerticalTabBar 自绘），加宽只增加留白与点击面积。
        "QTabBar::tab { background: #ECEFF1; color: #455A64;"
        "               border: 1px solid #CFD8DC; border-left: none;"
        "               margin: 2px 0px; padding: 12px 10px; min-width: 34px; }"
        "QTabBar::tab:selected { background: #1565C0; color: #FFFFFF;"
        "                        border: 1px solid #0D47A1; }");
    // ═══════════════════════════════════════════
    // ★ 2026-09-14 新增独立窗口页「计划分配表」
    //   客户要求：不往波次面板里加东西，单独一页展示该波次的计划分配情况。
    //   口径：**一行 = 一个产品（SKU）**，横向按格口展开成若干组；
    //     每组标题 = `034(正常分拣)` / `048(发货)`，组内 4 列：计划/已落/在途/余量；
    //     该产品不在某格口计划内 → 四列显示 `—`（而不是 0，避免误读为"计划 0 件"）。
    //   ★ 只读：唯一操作是「复制为文本」，不做任何写操作，避免误触改账。
    // ═══════════════════════════════════════════
    QGroupBox* grpPlanAlloc = new QGroupBox(QString::fromUtf8("计划分配表（每个产品 → 各格口计划/实际）"));
    m_pagePlanAlloc = grpPlanAlloc;   // ★ 本页根容器（前台判断用，见 MainWindow.h 说明）
    QVBoxLayout* paLayout = new QVBoxLayout(grpPlanAlloc);
    paLayout->setContentsMargins(6, 4, 6, 4);
    paLayout->setSpacing(4);

    // ── 汇总条 + 过滤/翻页 + 复制 ──
    {
        QHBoxLayout* bar = new QHBoxLayout();
        bar->setSpacing(8);
        m_lblPlanAllocSum = new QLabel(QString::fromUtf8("当前无运行波次"));
        m_lblPlanAllocSum->setStyleSheet("font-size: 13px; font-weight: bold; color: #37474F;");
        m_lblPlanAllocSum->setTextInteractionFlags(Qt::TextSelectableByMouse);
        bar->addWidget(m_lblPlanAllocSum, 1);

        QLabel* lbFilter = new QLabel(QString::fromUtf8("产品/SKU："));
        lbFilter->setStyleSheet("font-size: 12px;");
        bar->addWidget(lbFilter);
        m_editPlanAllocSku = new QLineEdit();
        m_editPlanAllocSku->setPlaceholderText(QString::fromUtf8("过滤（回车生效，留空=全部）"));
        m_editPlanAllocSku->setFixedWidth(200);
        m_editPlanAllocSku->setStyleSheet("font-size: 12px;");
        bar->addWidget(m_editPlanAllocSku);

        QLabel* lbPage = new QLabel(QString::fromUtf8("页："));
        lbPage->setStyleSheet("font-size: 12px;");
        bar->addWidget(lbPage);
        m_spinPlanAllocPage = new QSpinBox();
        m_spinPlanAllocPage->setRange(1, 1);
        m_spinPlanAllocPage->setFixedWidth(70);
        m_spinPlanAllocPage->setStyleSheet("font-size: 12px;");
        bar->addWidget(m_spinPlanAllocPage);

        QPushButton* btnCopy = new QPushButton(QString::fromUtf8("复制为文本"));
        btnCopy->setMinimumHeight(24);
        btnCopy->setStyleSheet("font-size: 12px; padding: 2px 10px;");
        btnCopy->setToolTip(QString::fromUtf8("把当前页（含格口明细列）导出到剪贴板，便于现场抄清单/核对"));
        bar->addWidget(btnCopy);

        // 过滤生效：重填表格（不重新取快照，避免无谓开销）
        connect(m_editPlanAllocSku, &QLineEdit::returnPressed, this, [this]() {
            m_planAllocFilter = m_editPlanAllocSku ? m_editPlanAllocSku->text().trimmed() : QString();
            m_planAllocPage = 1;
            if (m_spinPlanAllocPage) m_spinPlanAllocPage->setValue(1);
            renderPlanAllocRows();
        });
        connect(m_spinPlanAllocPage, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
            m_planAllocPage = v;
            renderPlanAllocRows();
        });
        connect(btnCopy, &QPushButton::clicked, this, [this]() {
            if (!m_tblPlanAlloc) return;
            QStringList lines;
            QStringList heads;
            for (int c = 0; c < m_tblPlanAlloc->columnCount(); ++c)
                if (m_tblPlanAlloc->horizontalHeaderItem(c))
                    heads << m_tblPlanAlloc->horizontalHeaderItem(c)->text();
            lines << heads.join("\t");
            for (int r = 0; r < m_tblPlanAlloc->rowCount(); ++r)
            {
                QStringList cells;
                for (int c = 0; c < m_tblPlanAlloc->columnCount(); ++c)
                    cells << (m_tblPlanAlloc->item(r, c) ? m_tblPlanAlloc->item(r, c)->text() : QString());
                lines << cells.join("\t");
            }
            QApplication::clipboard()->setText(lines.join("\n"));
            appendLog(QString::fromUtf8("[计划分配表] 已复制 %1 行到剪贴板").arg(m_tblPlanAlloc->rowCount()));
        });
        paLayout->addLayout(bar);
    }

    // ── 主表 ──
    m_tblPlanAlloc = new QTableWidget();
    m_tblPlanAlloc->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tblPlanAlloc->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tblPlanAlloc->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tblPlanAlloc->setAlternatingRowColors(true);
    m_tblPlanAlloc->verticalHeader()->setVisible(false);
    m_tblPlanAlloc->verticalHeader()->setDefaultSectionSize(26);
    m_tblPlanAlloc->horizontalHeader()->setHighlightSections(false);
    m_tblPlanAlloc->setStyleSheet(
        "QTableWidget { font-size: 12px; }"
        "QTableWidget::item { padding: 1px 4px; }"
        "QHeaderView::section { background-color: #e0e0e0; font-weight: bold; padding: 3px; }");
    m_tblPlanAlloc->setToolTip(QString::fromUtf8(
        "一行 = 一个产品（SKU）；每个格口一组「计划/已落/在途/余量」。\n"
        "· 计划 = H4 对该产品在该格口下发的件数（同格口多行累加）\n"
        "· 已落 = PLC 确认落入该格口的去重件数（唯一权威）\n"
        "· 在途 = 已下发 PLC、落格反馈未到的件数（占用额度，防同时两件在线超计划）\n"
        "· 余量 = 计划 − 已落 − 在途；余量 0 → 后续件改投异常口\n"
        "· 类型：正常分拣 / 发货（异常口为特殊口，不参与产品计划）\n"
        "· 格口标题颜色：黄=未绑定容器  红=满箱未重绑(禁用)  橙=物理锁格"));
    paLayout->addWidget(m_tblPlanAlloc, 1);

    m_lblPlanAllocHint = new QLabel(QString::fromUtf8(
        "提示：默认按计划件数降序、每页 100 行；格口超过 12 个时表格只展开前 12 个（「格口明细」列始终完整）。"));
    m_lblPlanAllocHint->setStyleSheet("font-size: 11px; color: #78909C;");
    m_lblPlanAllocHint->setWordWrap(true);
    paLayout->addWidget(m_lblPlanAllocHint);

    // ═══════════════════════════════════════════
    // ★ 2026-09-17 现场要求：**页签顺序调整**为
    //     ① 容器绑定状态 ② 运行日志 ③ 波次数据历史记录 ④ 分拣记录查询
    //   （原顺序：容器绑定状态 / 分拣记录查询 / 波次数据历史记录 / 计划分配表 / 实时面板 / 运行日志）
    //   两个可选页（受 XML 开关控制，默认隐藏）统一排在**最后**，不干扰上述四个固定页的顺序：
    //     ⑤ 计划分配表（<showPlanAllocPage>） ⑥ 实时面板（<showLivePage>）
    //   页签文字仍逐字竖排（VerticalTabBar），仅顺序变化；`setCurrentIndex(0)` 依旧默认落在「容器绑定状态」。
    // ═══════════════════════════════════════════
    m_tabMain->addTab(grpBinding,   QString::fromUtf8("容器绑定状态"));   // ①
    m_tabMain->addTab(grpLog,       QString::fromUtf8("运行日志"));       // ②
    m_tabMain->addTab(grpUnfinished,QString::fromUtf8("波次数据历史记录")); // ③
    m_tabMain->addTab(grpQuery,     QString::fromUtf8("分拣记录查询"));   // ④
    // ★ 2026-09-17 现场要求：两个可选页由 XML 开关控制，且**仅启动时读取一次**
    //   （<showPlanAllocPage> / <showLivePage>，默认 false = 隐藏；改了要重启才生效）。
    //   隐藏时不加页签：
    //     · 计划分配表：refreshPlanAllocPage 的"本页不在前台直接返回"判定自然恒成立 → 零开销；
    //     · 实时面板：连表格都不创建（m_tblLive=nullptr）→ 更新路径全部跳过。
    {
        const AppConfig& cfgUi = ConfigManager::instance()->config();
        if (cfgUi.showPlanAllocPage)
            m_tabMain->addTab(grpPlanAlloc, QString::fromUtf8("计划分配表"));   // ⑤
        else
            WCS_LOG_INFO("启动：按配置隐藏「计划分配表」页（showPlanAllocPage=false；改 XML 需重启生效）");
        if (grpLive)
            m_tabMain->addTab(grpLive, QString::fromUtf8("实时面板"));          // ⑥
        else
            WCS_LOG_INFO("启动：按配置隐藏「实时面板」页（showLivePage=false；改 XML 需重启生效）");
    }
    // 默认选中「容器绑定状态」（第 0 页）——现场首先看绑定
    m_tabMain->setCurrentIndex(0);
    // ★ 2026-09-14 切到「计划分配表」页时立即取一次快照（不等 2 秒节流），现场点开即见最新数据
    connect(m_tabMain, &QTabWidget::currentChanged, this, [this](int) {
        if (m_tabMain && m_pagePlanAlloc && m_tabMain->currentWidget() == m_pagePlanAlloc)
        {
            m_planAllocVersion = -1;      // 强制重建
            refreshPlanAllocPage(true);
        }
        // ★ 2026-09-17：切到「运行日志」页时立即把效率面板宽度对齐到「波次信息」面板
        //   （面板刚变为可见，此时才拿得到真实几何；不等 1 秒定时器）
        syncLogEffPanelWidth();
    });
    vsplit->addWidget(m_tabMain);

    // 垂直分配：第一行按内容（stretch 0），第二行（标签页）吃满剩余高度
    vsplit->setStretchFactor(0, 0);
    vsplit->setStretchFactor(1, 1);
    vsplit->setSizes({ 280, 700 });   // 初始高度（窗口尺寸变化时按比例缩放）

    mainLayout->addWidget(vsplit, 1);
}

// ============================================================================
// ★ 2026-09-13 实时面板（落格反馈数据）行操作
//   设计要点（性能）：所有操作都是 O(新增行数)，不查数据库、不做全表扫描；
//   占位索引 m_livePendingRows / m_liveProvisionedAt 只在"插入行/补全/裁剪"时维护，
//   因此主线程（业务关键路径所在线程）在实时面板上的开销可控且可测。
// ============================================================================

// 行裁剪：超过 LIVE_TABLE_MAX_ROWS 行时从尾部删（最旧），并整体重建占位索引
void MainWindow::livePanelTrimRows()
{
    if (!m_tblLive) return;
    if (m_tblLive->rowCount() <= LIVE_TABLE_MAX_ROWS) return;

    QSet<int> removedRows;
    while (m_tblLive->rowCount() > LIVE_TABLE_MAX_ROWS)
    {
        const int last = m_tblLive->rowCount() - 1;
        removedRows.insert(last);
        m_tblLive->removeRow(last);
    }
    // 行号整体位移：简单可靠的做法是按"仍存在的占位行"重建索引
    livePanelRebuildPendingIndex();
}

// 按当前表格内容重建占位索引（裁剪后行号全部失效，必须重建）
//   占位判定 = 状态列仍为"待落格"且 EPC 列与 key 一致（不需额外数据结构，避免失步）
void MainWindow::livePanelRebuildPendingIndex()
{
    // 先记下各 EPC 的占位创建时刻，重建后回填（否则超时判定会丢失起点）
    QHash<QString, qint64> atMs = m_livePendingAtMs;
    m_livePendingRows.clear();
    m_livePendingAtMs.clear();
    if (!m_tblLive) return;

    const QString pendingText = QString::fromUtf8(LIVE_STATUS_PENDING);
    for (int r = 0; r < m_tblLive->rowCount(); ++r)
    {
        QTableWidgetItem* st = m_tblLive->item(r, 7);
        QTableWidgetItem* ep = m_tblLive->item(r, 2);
        if (!st || !ep) continue;
        if (st->text() != pendingText) continue;      // 已补全/已打标 → 不再是占位
        const QString epc = ep->text();
        if (!m_livePendingRows.contains(epc))         // 同一 EPC 多条占位时保留最早一行（行号较大=较旧）
            m_livePendingRows.insert(epc, r);
        if (!m_livePendingAtMs.contains(epc))
            m_livePendingAtMs.insert(epc, atMs.value(epc, QDateTime::currentMSecsSinceEpoch()));
    }
}

// ★ 2026-09-15 原始报文留痕（界面层）：记住每个 EPC 最近一帧的原文与识别前 EPC
//   用途：实时面板 EPC 单元格鼠标悬停直接看到整帧原文，现场无需翻日志/查库
//   有界缓存：只保留最近 LIVE_FRAME_NOTE_MAX 件，避免长期运行内存增长
void MainWindow::rememberLiveFrameNote(const QString& epc, const QString& epcRaw,
                                       const QString& rawFrame, const QString& seq,
                                       const QString& devCode)
{
    if (epc.isEmpty()) return;
    const QString note = QString::fromUtf8(
        "EPC（参与分拣）：%1\n识别前原文：%2\n设备编码：%3  流水号：%4\n原始报文：%5")
        .arg(epc)
        .arg(epcRaw.isEmpty() ? QString::fromUtf8("（未发生识别/归一，与 EPC 相同）") : epcRaw)
        .arg(devCode.isEmpty() ? QStringLiteral("-") : devCode)
        .arg(seq.isEmpty() ? QStringLiteral("-") : seq)
        .arg(rawFrame.isEmpty() ? QString::fromUtf8("（无整帧原文：HTTP 直推入口）") : rawFrame);

    m_liveFrameNotes.insert(epc, note);
    m_liveFrameNoteOrder.append(epc);
    while (m_liveFrameNoteOrder.size() > LIVE_FRAME_NOTE_MAX)
    {
        const QString oldest = m_liveFrameNoteOrder.takeFirst();
        m_liveFrameNotes.remove(oldest);
    }
}

// RFID 推送先到：插入"待落格"占位行
void MainWindow::livePanelInsertPendingRow(const QString& epc, const QStringList& cells)
{
    if (!m_tblLive || epc.isEmpty()) return;

    m_tblLive->insertRow(0);
    const int n = qMin(cells.size(), m_tblLive->columnCount());
    for (int c = 0; c < n; ++c)
    {
        QTableWidgetItem* it = new QTableWidgetItem(cells.at(c));
        if (c == 2) { QFont f = it->font(); f.setFamily("Consolas"); it->setFont(f); }
        it->setTextAlignment((c == 0 || c == 1) ? int(Qt::AlignHCenter | Qt::AlignVCenter)
                                                : int(Qt::AlignLeft | Qt::AlignVCenter));
        if ((c == 3 || c == 4 || c == 5) && cells.at(c) == QStringLiteral("--"))
            it->setForeground(QColor("#9E9E9E"));   // SKU/格口/容器尚未知 → 置灰
        if (c == 7) it->setForeground(QColor("#1976D2"));   // 待落格：蓝色
        // ★ 2026-09-15 原始报文留痕：EPC 单元格悬停显示整帧原文与识别前 EPC
        if (c == 2)
        {
            const QString note = m_liveFrameNotes.value(epc);
            if (!note.isEmpty())
                it->setToolTip(note + QString::fromUtf8("\n（双击行可查看该 EPC 全信息与全部原始帧）"));
        }
        m_tblLive->setItem(0, c, it);
    }

    // 已有占位行整体下移一行 → 索引同步 +1
    for (auto it = m_livePendingRows.begin(); it != m_livePendingRows.end(); ++it)
        it.value() += 1;
    m_livePendingRows.insert(epc, 0);
    m_livePendingAtMs.insert(epc, QDateTime::currentMSecsSinceEpoch());

    livePanelTrimRows();
    // 行数少时保持最新可见；高频场景下不抢占操作员的滚动位置
    if (m_tblLive->rowCount() <= 40)
        m_tblLive->scrollToTop();
}

// PLC 落格反馈到达：命中占位行 → 就地补全；否则新增一行（占位被裁剪/已被补全时）
void MainWindow::livePanelApplyFeedback(const QString& epc, const QStringList& cells, bool bad)
{
    if (!m_tblLive || epc.isEmpty()) return;

    int row = -1;
    const int pendingRow = m_livePendingRows.value(epc, -1);
    if (pendingRow >= 0 && pendingRow < m_tblLive->rowCount())
    {
        QTableWidgetItem* st = m_tblLive->item(pendingRow, 7);
        QTableWidgetItem* ep = m_tblLive->item(pendingRow, 2);
        if (st && ep && ep->text() == epc && st->text() == QString::fromUtf8(LIVE_STATUS_PENDING))
            row = pendingRow;
    }

    if (row < 0)
    {
        // 无占位可补（RFID 帧未到/占位已被裁剪/同一 EPC 已有落格行）→ 新增一行，序号在此分配
        m_tblLive->insertRow(0);
        row = 0;
        for (auto it = m_livePendingRows.begin(); it != m_livePendingRows.end(); ++it)
            it.value() += 1;
    }
    else
    {
        m_livePendingRows.remove(epc);   // 已补全，不再是占位
        m_livePendingAtMs.remove(epc);
    }

    // 写入 8 列；补全占位时保留占位行原有序号（更符合"这件第几次被扫到"的直觉）
    const bool keepSeq = (row != 0);
    const int n = qMin(cells.size(), m_tblLive->columnCount());
    for (int c = 0; c < n; ++c)
    {
        if (c == 0 && keepSeq && m_tblLive->item(row, 0))
            continue;
        QTableWidgetItem* it = new QTableWidgetItem(cells.at(c));
        if (c == 2) { QFont f = it->font(); f.setFamily("Consolas"); it->setFont(f); }
        it->setTextAlignment((c == 0 || c == 1) ? int(Qt::AlignHCenter | Qt::AlignVCenter)
                                                : int(Qt::AlignLeft | Qt::AlignVCenter));
        if ((c == 3 || c == 5) && cells.at(c) == QStringLiteral("--"))
            it->setForeground(QColor("#9E9E9E"));
        if (c == 7)
        {
            it->setForeground(bad ? QColor("#D32F2F") : QColor("#2E7D32"));
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
        }
        m_tblLive->setItem(row, c, it);
    }
    livePanelTrimRows();
}

// 每秒（onRefreshTimer）：给长期未补全的占位行打标，避免一直显示"待落格"造成误解
void MainWindow::refreshLivePanelPendingRows()
{
    if (!m_tblLive || m_livePendingRows.isEmpty() || !m_pServer) return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const int timeoutMs = PLC_SEND_TIMEOUT_MS;
    QList<QString> done;
    for (auto it = m_livePendingRows.constBegin(); it != m_livePendingRows.constEnd(); ++it)
    {
        const QString epc = it.key();
        const int row = it.value();
        if (row < 0 || row >= m_tblLive->rowCount()) { done.append(epc); continue; }
        QTableWidgetItem* st = m_tblLive->item(row, 7);
        QTableWidgetItem* ep = m_tblLive->item(row, 2);
        if (!st || !ep || ep->text() != epc) { done.append(epc); continue; }
        if (st->text() != QString::fromUtf8(LIVE_STATUS_PENDING)) { done.append(epc); continue; }

        // 等待时长以"RFID 推送时刻"为起点（主线程维护，跨日也准确）
        const qint64 pushedAt = m_livePendingAtMs.value(epc, 0);
        if (pushedAt <= 0) continue;
        const qint64 waitedMs = nowMs - pushedAt;
        const bool inFlight = m_pServer->isPlcSendInFlight(epc);
        // 已不在途 → 指令已下发且已过在途窗口（或从未下发）：按等待时长分级打标
        if (!inFlight && waitedMs > timeoutMs)
        {
            st->setText(QString::fromUtf8("未落格（无反馈）"));
            st->setForeground(QColor("#9E9E9E"));
            done.append(epc);
        }
        else if (waitedMs > 12 * timeoutMs)
        {
            st->setText(QString::fromUtf8("待落格（超时未反馈）"));
            st->setForeground(QColor("#EF6C00"));
            done.append(epc);
        }
    }
    for (const QString& epc : done)
    {
        m_livePendingRows.remove(epc);
        m_livePendingAtMs.remove(epc);
    }
}

// ★ 2026-09-13 EPC 全信息窗（异常弹窗与查询结果共用）
void MainWindow::showEpcDetail(const QString& epc)
{
    if (epc.isEmpty() || !m_pQueryDb) return;
    EpcDetailDialog dlg(m_pQueryDb, epc, this);
    dlg.exec();
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
        // ★ 2026-09-15 EPC 识别长度（响应侧归一与推送侧同源同参数，热生效）
        m_pClient->setEpcTruncateLen(cfg.rfidEpcTruncateLen);
    }
    if (m_pServer)
    {
        if (m_pServer->waveManager())
        {
            m_pServer->waveManager()->setWaveTimeoutMin(cfg.waveTimeoutMin);
            m_pServer->waveManager()->setMaxRetry(cfg.maxRetryCount);
        }
        m_pServer->setExpectedBindCount(cfg.expectedBindCount);
        // ★ 2026-09-15 EPC 识别长度热生效（改配置点「保存并生效」即生效，无需重启）
        m_pServer->setEpcTruncateLen(cfg.rfidEpcTruncateLen);
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

    WCS_LOG_INFO("配置热生效应用完成（回传URL/AppKey/method、环境、HTTP超时、RFID查询/心跳、波次参数、期望绑定数、EPC识别长度）");
    appendLog("[配置] 热生效应用完成：回传 URL/AppKey/method、环境开关、HTTP超时、RFID查询/心跳、"
              "波次超时/重试、期望绑定数、EPC识别长度 已按新配置更新（端口/IP/线程池类需重启生效）");
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

    // ══════════════════════════════════════════════════════════════════════════
    // ★ 2026-09-15 需求①：每次开启软件 = 新任务状态（容器绑定面板全为"未绑定"）
    // ★ 2026-09-16 现场需求④（根因修复）：**不再从 XML 装载任何绑定** —— 面板只反映"本会话的绑定"
    //
    //   现场症状：开机后格口绑定面板显示一堆箱号（历史波次"占用"了绑定显示）。
    //   根因：`bindingUpdated` → ConfigManager::save() 会把**当时内存里的绑定**写回
    //         config/http_server.xml 的 <binding> 节点（一直在写），而启动时这里曾用
    //         "无未结束波次 + DB 无 active 绑定 → 采用 XML 预绑定"作为兜底 ——
    //         于是上一会话残留的 <binding> 被当成本次任务的绑定装载进面板。
    //         实测：本仓库 release_WcsHttpServer/config/http_server.xml 存有 43 条 <binding>，
    //         而 DB 侧 grid_box_bind 的 active 行数为 0 —— 面板显示的箱号全部来自该残留 XML。
    //
    //   现行口径：**绑定的唯一来源 = 本会话收到的 H6（或切回某个未结束波次时按其记录恢复）**；
    //     · XML 里的 <binding> 视为历史遗留镜像，**只写不读**（保留写入是为了不破坏既有落盘路径）；
    //     · 启动一律清空内存绑定（新任务状态），随后 startReceive → restoreWaveFromDB()
    //       把 DB 中残留的 active 行归档留痕（只归档、不改归属），因此面板必然为"未绑定"。
    // ══════════════════════════════════════════════════════════════════════════
    {
        const int xmlPresetCount = cfg.containerBindings.size();
        const int unfinishedCnt  = m_pServer->getUnfinishedWaves().size();
        const int activeBindCnt  =
            (m_pServer->sortingDb() && m_pServer->sortingDb()->isOpen()
                 ? m_pServer->sortingDb()->getAllActiveBinds().size()
                 : 0);

        m_pServer->loadContainerBindings(QMap<QString, QString>());   // 显式清空 = 新任务状态
        // ★ 同步清空内存中的配置镜像：否则旧值会在随后的 bindingUpdated 保存里被再写回 XML
        //   （这里不主动 save()：避免干扰 ConfigManager 既有的节流保存与"自写哈希"机制）
        cfg.containerBindings.clear();

        appendLog(QString::fromUtf8(
            "[容器绑定] 启动为新任务状态：不装载 XML 绑定（本次文件内 %1 个已忽略）；"
            "未结束波次 %2 个 / DB active 绑定 %3 个将由「开始接收任务」时归档保留"
            "（切回该波次时随波次一并恢复）")
            .arg(xmlPresetCount).arg(unfinishedCnt).arg(activeBindCnt));
        m_bindingDirty = true;  // ★ 初始加载后标记为脏，开始接收后首次刷新时更新面板
    }

    // ★ 设置期望绑定数量（从配置文件加载，默认1）
    m_pServer->setExpectedBindCount(cfg.expectedBindCount);

    // ★ 2026-09-15 EPC 识别长度（XML rfidEpcTruncateLen，默认 24）→ 注入 RFID 解析链路
    m_pServer->setEpcTruncateLen(cfg.rfidEpcTruncateLen);

    // ★ 回传客户端配置（H7/H8 回传 + RFID SKU-EPC 绑定查询）
    m_pClient->setUrl(cfg.activeFeedbackUrl());
    m_pClient->setEndUrl(cfg.activeEndFeedbackUrl());  // ★ 完结回传专用 URL
    m_pClient->setAppkey(cfg.activeAppkey());
    m_pClient->setFeedbackMethod(cfg.feedbackMethod);        // ★ 满箱/锁格/波次完成回传 method
    m_pClient->setEndFeedbackMethod(cfg.feedbackEndMethod);  // ★ 完结回传(H8) method
    m_pClient->setTimeout(cfg.httpTimeoutMs);
    m_pClient->setRfidQueryUrl(cfg.rfidQueryUrl);  // ★ RFID SKU-EPC 绑定查询 URL
    m_pClient->setRfidAppkey(cfg.rfidAppkey);      // ★ RFID 查询鉴权 AppKey
    // ★ 2026-09-15 EPC 识别长度（绑定查询响应侧归一；与推送侧同源，防缓存键不匹配）
    m_pClient->setEpcTruncateLen(cfg.rfidEpcTruncateLen);
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
        refreshFullboxCountLabel();   // ★ 需求④：失败/成功判定都会改变"本波次满箱回传"统计
    });

    // ★ 2026-09-16 需求④：某条 H7 报文**最终失败**（重试耗尽）→ 只对"本次一键生成的 msgId"计失败
    //   连接方式：HttpServer 在主线程发信号（onFullboxReplyFinished 走 reportResult 回主线程），
    //   默认 AutoConnection 即为直接调用，安全；此处显式用 QueuedConnection 兜住跨线程可能。
    connect(m_pServer, &HttpServer::fullboxMessageFailed, this, &MainWindow::onFullboxMessageFailed,
            Qt::QueuedConnection);

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

    // ★ 2026-09-17 绑定落库失败即时告警（跨线程信号 → 主线程槽）：
    //   H6 只更新了内存绑定、DB 行没写成 → 切回该波次时无法恢复该绑定。
    //   现场必须当场知道（此前只有 data.log 有记录，界面完全看不出来）。
    connect(m_pServer, &HttpServer::bindPersistFailed, this, &MainWindow::onBindPersistFailed);

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
        // ★ 2026-09-13 实时面板：同一次批量的反馈就地补全/新增「落格反馈数据（实时）」行
        connect(m_pPlcMgr, &PlcManager::plcFeedbackBatch, this,
            [this](const QVector<PlcFeedbackEntry>& entries) {
                if (entries.isEmpty()) return;

                // ── 合并实时表：PLC 落格反馈 → 补全 RFID 占位行 或 新增行 ──
                if (m_tblLive)
                {
                    const bool heavy = entries.size() > 16;
                    if (heavy) m_tblLive->setUpdatesEnabled(false);
                    QElapsedTimer batchTimer;
                    batchTimer.start();
                    for (const auto& e : entries)
                    {
                        QString statusTxt;
                        bool bad = false;
                        // ★ 2026-09-09 需求3：状态码旁附加信息描述（0/1/2/3 为已知语义；7/8 待客户确认，先给占位描述）
                        switch (e.status)
                        {
                        case 0: statusTxt = QStringLiteral("0 (3字段无状态)");      break;
                        case 1: statusTxt = QStringLiteral("1 成功");                 break;
                        case 2: statusTxt = QStringLiteral("2 无格口");   bad = true; break;
                        case 3: statusTxt = QStringLiteral("3 信息不全"); bad = true; break;
                        case 7: statusTxt = QStringLiteral("7 状态码7(待确认含义)");  break;
                        case 8: statusTxt = QStringLiteral("8 状态码8(待确认含义)");  break;
                        default: statusTxt = QString::fromUtf8("状态码%1(未知)").arg(e.status); break;
                        }
                        // 重投件标注（同一 EPC 本波次重投次数）
                        const int resendTimes = m_pServer ? m_pServer->rescanResendTimes(e.code) : 0;
                        if (resendTimes > 0)
                            statusTxt += QString::fromUtf8("（重投%1次）").arg(resendTimes);

                        // 对应SKU：优先 EpcCache（RFID 查询结果，TTL 300s），取不到显示 --
                        const QString sku = m_pServer ? m_pServer->getSkuByEpc(e.code) : QString();
                        // 容器号：落格时刻该格口的绑定容器（内存绑定表）
                        const QString box = m_pServer ? m_pServer->containerForGrid(e.grid) : QString();
                        const QString gridKey = gridKeyOf(e.grid);

                        const QStringList cells = QStringList()
                            << QString()   // 序号：补全占位时保留原序号；新增行时填充
                            << QDateTime::fromMSecsSinceEpoch(e.timestampMs).toString("HH:mm:ss")
                            << e.code
                            << (sku.isEmpty() ? QStringLiteral("--") : sku)
                            << gridKey
                            << (box.isEmpty() ? QStringLiteral("--") : box)
                            << (e.lastCar.isEmpty() ? e.car
                                                    : QString::fromUtf8("首:%1 尾:%2").arg(e.firstCar, e.lastCar))
                            << statusTxt;
                        livePanelApplyFeedback(e.code, cells, bad);
                    }
                    if (heavy) m_tblLive->setUpdatesEnabled(true);
                    m_tblLive->viewport()->update();
                    // ★ 性能核验：实时面板单批插入耗时（>50ms 说明 UI 已成为瓶颈，会在日志中暴露）
                    const qint64 uiMs = batchTimer.elapsed();
                    if (uiMs > 50)
                    {
                        static qint64 lastWarnMs = 0;
                        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                        if (nowMs - lastWarnMs > 10000)   // 限流：最多每 10s 一条
                        {
                            lastWarnMs = nowMs;
                            appendLog(QString::fromUtf8("[性能] 实时面板插入 %1 行耗时 %2ms（>50ms，UI 可能成为瓶颈）")
                                .arg(entries.size()).arg(uiMs), true);
                        }
                    }
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

    // ★ 2026-09-13 实时面板 —— RFID 推送接入（"先到先占一行"）
    //   RfidPushClient::rfidPushReceived 在 HP-Socket 工作线程 emit（body.data[].epc/carNum），
    //   以 QueuedConnection 回主线程：先建"待落格"占位行；PLC 落格反馈到达后就地补全同一行。
    //   这样既保留"EPC 一到就能在面板看到"，又保证同一件不会出现两行。
    if (m_pServer && m_pServer->rfidPush())
    {
        connect(m_pServer->rfidPush(), &RfidPushClient::rfidPushReceived, this,
            [this](const QJsonObject& body) {
                if (!m_tblLive) return;
                const QJsonArray arr = body.value("data").toArray();
                if (arr.isEmpty()) return;
                const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
                const bool heavy = arr.size() > 16;
                if (heavy) m_tblLive->setUpdatesEnabled(false);
                for (const QJsonValue& v : arr)
                {
                    const QJsonObject o = v.toObject();
                    const QString epc = o.value("epc").toString();
                    if (epc.isEmpty()) continue;   // 与业务侧一致：NOREAD 等空 EPC 不展示
                    // ★ 2026-09-15 原始报文留痕：把本帧原文与识别前 EPC 记下来，
                    //   供 EPC 单元格鼠标悬停查看（不查库、不影响主链路）
                    rememberLiveFrameNote(epc,
                        o.value("epcRaw").toString(), o.value("raw").toString(),
                        o.value("seq").toString(), o.value("devCode").toString());
                    // 对应SKU：优先 EpcCache（RFID SKU 绑定查询结果；查询未回来时显示 -- 并置灰）
                    const QString sku = m_pServer ? m_pServer->getSkuByEpc(epc) : QString();
                    // 格口号/容器号此刻未知 → 占位留空；状态标记为"待落格"
                    livePanelInsertPendingRow(epc, QStringList()
                        << QString::number(++m_liveSeq)
                        << ts
                        << epc
                        << (sku.isEmpty() ? QStringLiteral("--") : sku)
                        << QStringLiteral("--")
                        << QStringLiteral("--")
                        << o.value("carNum").toString()
                        << QString::fromUtf8(LIVE_STATUS_PENDING));
                }
                if (heavy) m_tblLive->setUpdatesEnabled(true);
                m_tblLive->viewport()->update();
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
    refreshFullboxCountLabel();   // ★ 2026-09-16 需求④：初始化「一键满箱」计数标签（无波次时显示"无运行波次"）
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
        m_btnStartStop->setStyleSheet(startStopButtonStyle("#FF9800", "#F57C00"));
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
        // ★ 2026-09-16 需求②⑦：复位**整格底色为"未绑定=无色"** + 箱号文字回深灰 `--`（隐藏格无控件，判空跳过）；
        //   计数口径同步改为"可见格口数"（未配置异常口时=66，配置 66 时=65）；字号保持 13px
        for (int i = 0; i < BINDING_SLOT_COUNT; ++i)
        {
            if (m_bindingLabels[i])
                m_bindingLabels[i]->setStyleSheet(
                    "font-size: 10px; color: white; border-radius: 6px; background-color: transparent;");
            if (m_bindingBoxLabels[i])
            {
                m_bindingBoxLabels[i]->setStyleSheet(
                    QString("font-size: 13px; color: %1; font-weight: bold;"
                            " border: none; background: transparent;").arg(BP_TEXT_COLOR_PLAIN));
                m_bindingBoxLabels[i]->setText("--");
            }
        }
        for (int k = 0; k < m_bindingFrames.size(); ++k)
        {
            if (m_bindingFrames[k])
                m_bindingFrames[k]->setStyleSheet(k < m_bindingInitialStyles.size()
                    ? m_bindingInitialStyles[k]
                    : bpFrameStyle(BP_UNBOUND));   // 未绑定=无色
        }
        if (m_lblBoundCount)  m_lblBoundCount->setText("已绑定: 0");
        if (m_lblLockedCount) m_lblLockedCount->setText(QString::fromUtf8("已锁格: 0"));
        if (m_lblUnboundCount) m_lblUnboundCount->setText(
            QString("未绑定: %1").arg(visibleBindingSlotCount()));

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
            m_btnStartStop->setStyleSheet(startStopButtonStyle("#f44336", "#d32f2f"));
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
            // ★ 2026-09-16 需求④⑤：接收开始后刷新计数标签（本波次满箱回传）与 H7 失败下拉（仅本波次）
            refreshFullboxCountLabel();
            refreshFailedCombos();

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
                // ★ 2026-09-15 EPC 识别口径与原始报文留痕（现场一眼确认当前生效规则）
                {
                    const QString epcRule = (cfg.rfidEpcTruncateLen < 2)
                        ? QString::fromUtf8("不识别（rfidEpcTruncateLen<2：RFID 推送串原样使用）")
                        : QString::fromUtf8("只识别『A + %1 位数字』共 %2 位（rfidEpcTruncateLen；"
                                            "与开头是不是 A101 无关）")
                              .arg(cfg.rfidEpcTruncateLen - 1).arg(cfg.rfidEpcTruncateLen);
                    appendLog(QString(" EPC识别:         %1\n\n").arg(epcRule));
                    appendLog(QString::fromUtf8(" 原始报文留痕:    日志(log/Run/run.log) + 界面(实时面板悬停/EPC全信息弹窗) "
                                                "+ 数据库(rfid_raw 表，保留 %1 天)\n\n").arg(RFID_RAW_RETAIN_DAYS));
                }
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

    // ★ 2026-09-13 实时面板：给长期未补全的"待落格"行打标（只遍历占位集合，规模小）
    refreshLivePanelPendingRows();

    // ★ 2026-09-16 需求④：日志页右侧效率面板每秒一拍
    //   （面板懒创建前回调为空；面板不可见时 tick() 内部直接返回 → 开销为零）
    if (m_logEffTick) m_logEffTick();
    // ★ 2026-09-17 现场要求：效率统计面板宽度跟随「波次信息」面板（宽度未变时内部直接返回）
    syncLogEffPanelWidth();

    // ★ 2026-09-13 异常留痕计数：10 秒一次同步查询后缓存（面板每秒渲染只读缓存）
    //   这样"异常留痕(条)"既能实时更新，又不会让主线程每秒阻塞在 DB 查询上（分拣关键路径同线程）
    //   波次切换时立即刷新一次（下面的 orderCode 比较），保证换波次后数字不滞后
    {
        static int excTraceCounter = 0;
        static QString lastTraceOrder;
        WaveManager* wm = m_pServer->waveManager();
        const QString curOrder = wm ? wm->orderCode() : QString();
        const bool orderChanged = (curOrder != lastTraceOrder);
        if (orderChanged || (++excTraceCounter % 10 == 0))
        {
            lastTraceOrder = curOrder;
            if (m_pQueryDb && m_pQueryDb->isOpen() && !curOrder.isEmpty())
            {
                m_cachedExcTraceCount = m_pQueryDb->queryExceptions(curOrder, QString(), QString(),
                                                                    QString(), QString(),
                                                                    SORTING_QUERY_MAX_RESULTS).size();
            }
            else if (curOrder.isEmpty())
            {
                m_cachedExcTraceCount = -1;   // 无波次 → 面板显示 --
            }
            if (orderChanged)
            {
                onRefreshWaveRecords();       // 换波次/新波次：历史列表同步一次
                // ★ 2026-09-16 需求④⑤：换波次时同步刷新「一键满箱」计数（本次一键归零 + 本波次数值）
                //   与「H7 失败格口」下拉（内容只含本波次，换波次必须重建）
                refreshFullboxCountLabel();
                refreshFailedCombos();
            }
        }
    }

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
    // ★ 2026-09-13：波次面板改为"始终刷新"——停止接收/完结后仍需看到最终计数
    //   （尤其"异常口/异常留痕/波次时长"为终态对账数据，不能因 m_bRunning=false 而冻结）
    else if (m_pServer->waveManager())
    {
        updateWavePanel();
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

    // ★ 2026-09-13 口径显式化（客户需求：异常/分拣数量关系必须看得懂、异常要能及时清理）
    m_lblPlanQty->setText(QString("%1 件").arg(snap.orderQty));
    m_lblPlanQty->setToolTip(QString::fromUtf8(
        "计划件数 = WMS 下发波次头 orderQty（该波次计划分拣的总件数）"));

    m_lblSorted->setText(QString("%1 / %2").arg(snap.sortedCount).arg(snap.orderQty));
    m_lblSorted->setToolTip(QString::fromUtf8(
        "已分拣件数（件次口径） = PLC 落格反馈累计次数：\n"
        "  · 含重复反馈与重投落格（同一件被反馈多次就计多次）\n"
        "  · 与右侧「计划」对照可看进度"));
    m_lblSorted->setStyleSheet("font-size: 15px; font-weight: bold; color: #2E7D32;");   // ★ 面板字体放大 13→15

    // ★ 2026-09-13 客户口径：「处理」= 仍在异常口、尚未处理完的件数（去重 EPC）；
    //   「异常口」= 同一批待处理件（同源同值）——两者都会在该 EPC **成功落格**时同步 −1。
    m_lblException->setText(QString("%1 件").arg(snap.exceptionCount));
    m_lblException->setToolTip(QString::fromUtf8(
        "处理 = 仍在异常口、尚未处理完的件数（去重 EPC）：\n"
        "  · PLC 判定失败（状态2 无格口 / 状态3 信息不全）即 +1（同一 EPC 反复掉入只算 1 件）\n"
        "  · 该 EPC 之后**成功落格**即视为已处理 → 此数立即 −1（不做只增累计）\n"
        "  · 不计入「已分拣」；留痕（未匹配/未绑定/冲突/重扫等）不增加此数\n"
        "点右侧「查看处理」查看明细（双击行可看该 EPC 全信息）"));
    m_lblException->setStyleSheet(snap.exceptionCount > 0
        ? "font-size: 15px; font-weight: bold; color: #D32F2F;"
        : "font-size: 15px; font-weight: bold; color: #2196F3;");

    // ★ 异常口数值（同样 15px）；数量与左侧「处理」同源
    m_lblExcBin->setStyleSheet(snap.exceptionCount > 0
        ? "font-size: 15px; font-weight: bold; color: #FF6D00;"
        : "font-size: 15px; font-weight: bold; color: #2196F3;");
    m_lblExcBin->setText(QString("%1 件").arg(snap.exceptionCount));
    m_lblExcBin->setToolTip(QString::fromUtf8(
        "异常口 = 仍在异常口、尚未处理完的件数（去重 EPC，与左侧「处理」同一个量）：\n"
        "  · 一个 EPC 只记一次（重复掉入不重复计）\n"
        "  · **成功落格即视为已处理 → 本数同步减少**（不再只增不减）\n"
        "  · 需要「本波次曾掉入异常口的总量」做对账时，请看「留痕」或异常明细弹窗"));

    // 留痕条数（exception_record）：含"仅留痕、未真正入异常口"的记录
    // ★ 2026-09-13 性能保护：该值由 onRefreshTimer 以 10 秒周期刷新到 m_cachedExcTraceCount，
    //   面板每秒渲染时只读缓存——避免每秒一次同步 DB 查询占用主线程（分拣关键路径所在线程）
    const int excTrace = m_cachedExcTraceCount;
    m_lblExcTrace->setText(excTrace < 0 ? QString("--") : QString("%1 条").arg(excTrace));
    m_lblExcTrace->setToolTip(QString::fromUtf8(
        "留痕条数 = exception_record 记录数（含下列两类）：\n"
        "  · 计入处理的：plc_no_grid(无格口) / plc_info_incomplete(信息不全)\n"
        "  · 仅留痕（PLC 报成功、已计为已分拣，不增加处理数）：无匹配/无绑定/冲突/重扫超限/发送超时等\n"
        "点右侧「查看处理」查看明细与「是否计入 / 是否已闭环」标注"));

    m_lblSumLocation->setText(QString::number(snap.sumLocation));
    m_lblSumLocation->setToolTip(QString::fromUtf8(
        "分拣件数（去重 EPC） = 已落格的不重复实物件数：\n"
        "  · 「已分拣」是件次口径（含重复反馈/重投），本值是件口径，两者不同\n"
        "  · WMS 完结回传（H8）的 sumLocation 用此值"));

    // ★ 2026-09-13 新增：RFID 扫描次数 = RFID 推送 EPC 次数（重复 EPC 重复计数）
    const quint64 rfidScans = m_pServer->rfidPushTotal();
    m_lblRfidScanCount->setText(QString("%1 次").arg(rfidScans));
    m_lblRfidScanCount->setStyleSheet("font-size: 15px; font-weight: bold; color: #6A1B9A;");
    m_lblRfidScanCount->setToolTip(QString::fromUtf8(
        "RFID 扫描次数 = 本次运行累计收到的 RFID 推送 EPC 条数：\n"
        "  · 每推送一个 EPC 记 1 次；**同一个 EPC 重复推送重复计数**\n"
        "  · 不含空 EPC / NOREAD（未读到标签不计数）\n"
        "  · 只记推送次数，不代表分拣件数；跨波次不清零，程序重启归 0"));

    // 波次时长（mm:ss / hh:mm:ss）
    {
        const qint64 sec = qMax<qint64>(0, snap.elapsedSec);
        const qint64 h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
        m_lblElapsed->setText(h > 0 ? QString("%1:%2:%3").arg(h)
                                          .arg(m, 2, 10, QChar('0'))
                                          .arg(s, 2, 10, QChar('0'))
                                    : QString("%1:%2").arg(m, 2, 10, QChar('0'))
                                          .arg(s, 2, 10, QChar('0')));
        m_lblElapsed->setToolTip(QString::fromUtf8("当前波次已耗时（从波次注册起算）"));
    }

    // ★ 2026-09-13 客户要求：波次信息面板不再显示容器绑定数据 → 此处同步移除刷新逻辑
    //   （容器绑定状态在第 0 页标签页展示，含已绑定/已锁格/未绑定计数）

    // ★ 2026-09-13 超计划预警：数字 = 超计划条目数（格口+SKU 粒度），有值时红色加粗并激活「查看」
    refreshOverplanWarning();

    // ★ 2026-09-14 计划分配表页：内部自带"本页不在前台直接返回 + 2 秒节流 + 版本未变不重建"，
    //   因此这里每秒调用是安全的（空闲时只读一个原子版本号）
    refreshPlanAllocPage(false);

    // 查看处理按钮：无待处理件时置灰但保留可见（避免布局跳动）
    if (m_btnViewException)
    {
        m_btnViewException->setEnabled(snap.exceptionCount > 0 || excTrace > 0);
        m_btnViewException->setText(snap.exceptionCount > 0
            ? QString::fromUtf8("查看处理(%1)").arg(snap.exceptionCount)
            : (excTrace > 0 ? QString::fromUtf8("查看处理(%1条留痕)").arg(excTrace)
                            : QString::fromUtf8("查看处理")));
    }

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

    // ★ 2026-09-16 现场需求：状态显示改为**对应底色 + 白色文字**（做成色块标签）
    //   口径统一实现在 tests/BindingPanelPolicy.h::bpWaveStatusStyle（与自测同源）：
    //     蓝=已下发 / 绿=已绑定 / 橙=分拣中·满箱同步中 / 红=完结中·取消处理中·异常挂起 / 灰=空闲·终态
    //   ★ 标签需要 setText 之后再套样式（Qt 的 QLabel 背景只覆盖文字所在区域，宽度随文字变化）
    m_lblWaveStatus->setStyleSheet(bpWaveStatusStyle(snap.waveStatus));
    m_lblWaveStatus->setToolTip(bpWaveStatusLegend());

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
    m_btnStartStop->setStyleSheet(startStopButtonStyle("#4CAF50", "#45a049"));
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

// ============================================================================
// ★ 2026-09-17 只读查看选中波次的格口绑定（现场问题："该波次下的格口绑定状态与显示不会随切回回溯"）
//
//   为什么单独做入口（而不是只靠切回）：切回是有副作用的动作（切出当前波次），
//   而现场需要的只是"看一眼这个波次当时绑的是哪些箱子"。本入口：
//     · 不切换波次、不动绑定、不写任何数据；
//     · 取数 = 每格取**该波次内**最后一条绑定（SortingDatabase::getLastBindsByOrder），
//       与切回恢复口径、与列表「格口绑定」列**同一 SQL**（不会出现两套口径打架）；
//     · 终态波次也可查（数据不删除，"不允许切回"≠"不能回溯"）。
// ============================================================================
void MainWindow::onViewWaveBinds()
{
    if (!m_pServer || !m_tblWaveRecords) return;

    const int row = m_tblWaveRecords->currentRow();
    if (row < 0)
    {
        appendLog("[查看绑定] 请先在「波次数据历史记录」中选中一行波次", true);
        return;
    }
    QTableWidgetItem* it = m_tblWaveRecords->item(row, 0);
    if (!it) return;
    const QString orderCode = it->text().trimmed();
    if (orderCode.isEmpty()) return;

    const QVector<GridBoxBindRecord> rows = m_pServer->getWaveBindDetail(orderCode);

    // 明细摘要用真实性口径（不猜）：confirmed = 该格全表最后一条仍属本波次；active = 现场当前活跃绑定
    QJsonObject bs = m_pServer->getUnfinishedWaveSummary(orderCode);
    const int confirmedCnt = bs.contains("bindConfirmed") ? bs["bindConfirmed"].toInt() : rows.size();
    const int activeCnt    = bs.contains("bindActive")    ? bs["bindActive"].toInt()
                                                          : m_pServer->boundCount();

    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8("波次 %1 的格口绑定（只读）").arg(orderCode));
    // ★ 2026-09-17 现场反馈「查看绑定弹窗里看不到下面的 SQL / 表格被挤掉」：
    //   根因是**布局抢空间** —— 长 SQL 用 QLabel+wordWrap 会被折成十几行，
    //   与表格在同一列里互相挤压，620px 高的对话框容不下两者的最小高度，总有一块被裁掉。
    //   修法：① 对话框默认放大 + 给最小尺寸；② 表格给 stretch（它才是主体，可滚动）；
    //        ③ SQL 改用**固定高度只读文本框**（不换行、可横向滚动、可全选复制）→ 两块必然同时可见。
    dlg.resize(820, 760);
    dlg.setMinimumSize(560, 460);
    QVBoxLayout* lay = new QVBoxLayout(&dlg);

    // 头部摘要：本波次记录的格口数 / 其中已被更晚波次重新绑定 / 现场当前物理绑定
    QLabel* head = new QLabel(QString::fromUtf8(
        "波次 %1：本波次记录 %2 个格口（每格取该波次内最后一条绑定 = **切回时恢复的口径**）\n"
        "　　　　其中该格最后一条仍属本波次 %3 个，已被更晚波次重新绑定 %4 个\n"
        "当前运行波次的物理绑定：%5 个（与上面数字无关，仅供对照）")
        .arg(orderCode).arg(rows.size()).arg(confirmedCnt)
        .arg(qMax(0, rows.size() - confirmedCnt)).arg(activeCnt));
    head->setStyleSheet("font-size: 13px; color: #333;");
    head->setWordWrap(true);
    head->setToolTip(bpWaveBindDetailTooltip(rows.size(), confirmedCnt, activeCnt));
    lay->addWidget(head);

    if (rows.isEmpty())
    {
        // 无绑定记录：给一个**显眼的红框**（而不是一行小字），现场一眼就知道"这里本来就查不到东西"
        QLabel* empty = new QLabel(QString::fromUtf8(
            "本波次**没有绑定记录**（该波次可查的格口绑定 = 0 个）。\n\n"
            "含义：该波次从未收到带本波次号的 H6 绑定（H6 报文本身不含波次号，归属靠运行时判定），\n"
            "因此切回该波次时**恢复不出**该波次的绑定；若库中仍有当前活跃绑定，\n"
            "切回时只会按「物理当前绑定」显示（**不是本波次记录**），等 WMS 重发 H6 才会落到本波次名下。\n\n"
            "历史数据核查（只读、不改数据）：python docs\\wave_bind_audit.py"));
        empty->setStyleSheet("font-size: 13px; color: #B3261E; background: #FFF3F2;"
                             "border: 1px solid #E6B3AE; border-radius: 4px; padding: 10px;");
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        empty->setMinimumHeight(150);
        lay->addWidget(empty, 1);   // 无表格时由它吃满余量
    }
    else
    {
        QTableWidget* tbl = new QTableWidget(rows.size(), 5);
        tbl->setHorizontalHeaderLabels(QStringList()
            << QString::fromUtf8("格口") << QString::fromUtf8("容器号")
            << QString::fromUtf8("绑定时间") << QString::fromUtf8("解绑时间")
            << QString::fromUtf8("是否仍活跃"));
        tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
        tbl->horizontalHeader()->setStretchLastSection(true);
        tbl->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        tbl->setStyleSheet("QTableWidget { font-size: 13px; }");
        tbl->setMinimumHeight(160);   // ★ 保证表格至少能看到表头 + 数行（此前会被 SQL 标签挤掉）
        for (int r = 0; r < rows.size(); ++r)
        {
            const GridBoxBindRecord& b = rows[r];
            auto set = [&](int c, const QString& t) {
                QTableWidgetItem* cell = new QTableWidgetItem(t);
                cell->setFlags(cell->flags() & ~Qt::ItemIsEditable);
                tbl->setItem(r, c, cell);
            };
            set(0, b.gridNum);
            set(1, b.boxcode);
            set(2, b.bindTime.isEmpty() ? QString("--") : b.bindTime);
            set(3, b.unbindTime.isEmpty() ? QString("--") : b.unbindTime);
            set(4, b.active ? QString::fromUtf8("活跃") : QString::fromUtf8("已归档"));
        }
        lay->addWidget(tbl, 1);   // ★ 表格吃满余量（可滚动）
    }

    // 现场自查用的只读 SQL（与取数口径逐字一致，可直接在 sqlite 工具里执行）
    const QString sqlText = QString::fromUtf8(
        "-- 只读核对 SQL（= 本弹窗口径；把 X 换成上面的波次号）\n"
        "SELECT g1.grid_num, g1.boxcode, g1.active, g1.bind_time, g1.unbind_time\n"
        "  FROM grid_box_bind g1\n"
        " WHERE g1.order_code = 'X' AND g1.boxcode <> ''\n"
        "   AND g1.rowid = (SELECT MAX(g2.rowid) FROM grid_box_bind g2\n"
        "                    WHERE g2.grid_num = g1.grid_num AND g2.order_code = 'X')\n"
        " ORDER BY CAST(g1.grid_num AS INTEGER);");
    QLabel* sqlTitle = new QLabel(QString::fromUtf8("只读核对 SQL（与上方明细同一取数口径）："));
    sqlTitle->setStyleSheet("font-size: 12px; color: #555; margin-top: 4px;");
    lay->addWidget(sqlTitle);

    // ★ 固定高度只读文本框：不换行（横向滚动）、可全选复制 → 无论表格多少行都必然可见
    QPlainTextEdit* sqlBox = new QPlainTextEdit(sqlText);
    sqlBox->setReadOnly(true);
    sqlBox->setLineWrapMode(QPlainTextEdit::NoWrap);
    sqlBox->setFixedHeight(140);
    sqlBox->setStyleSheet("QPlainTextEdit { font-family: Consolas, 'Courier New', monospace;"
                          " font-size: 12px; color: #333; background: #F7F7F7;"
                          " border: 1px solid #DDDDDD; border-radius: 3px; }");
    lay->addWidget(sqlBox);

    QPushButton* btnCopySql = new QPushButton(QString::fromUtf8("复制 SQL"));
    btnCopySql->setMinimumHeight(28);
    connect(btnCopySql, &QPushButton::clicked, this, [this, sqlText]() {
        QApplication::clipboard()->setText(sqlText);
        appendLog("[查看绑定] 只读核对 SQL 已复制到剪贴板");
    });
    QPushButton* btnClose = new QPushButton(QString::fromUtf8("关闭"));
    btnClose->setMinimumHeight(28);
    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);
    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addWidget(btnCopySql);
    btnRow->addStretch();
    btnRow->addWidget(btnClose);
    lay->addLayout(btnRow);

    appendLog(QString("[查看绑定] 波次 %1：本波次绑定记录 %2 个格口（只读，未切换波次、未改数据）")
        .arg(orderCode).arg(rows.size()));
    dlg.exec();
}

// ★ 2026-09-17 绑定落库失败即时告警：H6 只改了内存、DB 行没写成
//   → 该绑定在"切回该波次"时恢复不出来（现场历史现象："面板显示已绑定、库里一行都没有"）。
//   此处红字 + 计数，运维可当场重发 H6 或排查磁盘/权限。
void MainWindow::onBindPersistFailed(const QString& grid, const QString& box, const QString& orderCode)
{
    ++m_bindPersistFailedCount;
    m_lastBindPersistFailed = QString::fromUtf8("格口%1→容器%2（波次:%3）")
        .arg(grid).arg(box).arg(orderCode.isEmpty() ? QString::fromUtf8("(归属待补齐)") : orderCode);

    appendLog(QString::fromUtf8(
        "[容器绑定] **落库失败** %1 —— 面板已显示该绑定，但数据库没有写入；"
        "切回该波次时**无法恢复**该绑定。请检查磁盘/权限，并让 WMS 重发 H6。")
        .arg(m_lastBindPersistFailed), true);

    if (m_lblBindPersistFailed)
    {
        m_lblBindPersistFailed->setText(QString::fromUtf8("⚠ 绑定未落库 %1 个（最近 %2）")
            .arg(m_bindPersistFailedCount).arg(m_lastBindPersistFailed));
        m_lblBindPersistFailed->setToolTip(QString::fromUtf8(
            "H6 绑定只更新了内存、数据库写入失败（详见 DataBase/data.log）。\n"
            "影响：切回该波次时按波次取不到这些绑定；现场表现为「面板已绑定、库里一行都没有」。\n"
            "处置：① 排查磁盘空间/文件权限；② 让 WMS 重发 H6；③ 用「查看绑定」核对库中记录。"));
        m_lblBindPersistFailed->setVisible(true);
    }
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

// ★ 2026-09-16 需求②：该格口是否在「容器绑定状态」面板中隐藏
//   口径（纯逻辑实现在 tests/BindingPanelPolicy.h::bpIsGridHidden）：
//     隐藏对象 = 配置的物理异常口（exceptionGrid，现场=66）——异常口只收超计划件、
//     不上传 WMS、也不参与产品计划，其绑定状态对操作员没有意义，故整格不渲染。
//     exceptionGrid 为空 / "0"（未配置）→ 不隐藏任何格口（与改造前一致，零回归）。
bool MainWindow::isGridHiddenInBindingPanel(int gridNum) const
{
    return bpIsGridHidden(gridNum, ConfigManager::instance()->config().exceptionGrid);
}

// ★ 2026-09-16 需求②：面板可见格口数（计数口径与之保持一致，避免"65 格却显示未绑定 66"）
int MainWindow::visibleBindingSlotCount() const
{
    return bpVisibleSlotCount(BINDING_SLOT_COUNT, ConfigManager::instance()->config().exceptionGrid);
}

void MainWindow::updateBindingPanel()
{
    if (!m_pServer) return;

    QMap<QString, QString> bindings = m_pServer->getContainerBindings();
    int boundCount = 0;
    int lockedCount = 0;          // ★ 2026-09-11：已锁格数量（满箱锁格）
    int pendingRebindCount = 0;   // ★ 2026-09-09：已物理解锁但仍等待 WMS 重绑(H6)的格口数

    // ★ 2026-09-16 需求⑦：整格底色 = 状态（配色/优先级取自 BindingPanelPolicy.h，与自测同源）；
    //   圆点已隐藏，故此处只设外框底色 + 箱号/状态文字
    //   ★ 本次更新：字号 11px → 13px（客户要求"格口绑定状态内的字体调大"）；
    //     文字色按状态取 —— 有色底（绿/橙/红）=白字；未绑定（无色底）=深灰字
    // slot = 格口数组下标（gridNum-1）；state = 四态；boxText = 箱号或状态文案
    auto paintGrid = [&](int slot, BindingPanelState state, const QString& boxText) {
        if (slot < 0 || slot >= BINDING_SLOT_COUNT) return;
        if (slot < m_bindingFrames.size() && m_bindingFrames[slot])
            m_bindingFrames[slot]->setStyleSheet(bpFrameStyle(state));   // 整格底色（未绑定=无色）
        QLabel* lblBox = m_bindingBoxLabels[slot];
        if (lblBox)
        {
            lblBox->setStyleSheet(QString("font-size: 13px; color: %1; font-weight: bold;"
                                          " border: none; background: transparent;")
                                      .arg(bpBoxTextColorOf(state)));
            lblBox->setText(boxText);
        }
    };

    for (int i = 0; i < BINDING_SLOT_COUNT; ++i)
    {
        int gridNum = i + 1;
        QString gridKey = QString("%1").arg(gridNum, GRID_KEY_PADDING, 10, QChar('0'));
        QString boxCode = bindings.value(gridKey, "");

        // ★ 需求②：隐藏格口（异常口）不渲染、不计数 —— 双保险（建格时已跳过）
        if (isGridHiddenInBindingPanel(gridNum)) continue;

        QLabel* lblStatus = m_bindingLabels[i];
        QLabel* lblBox    = m_bindingBoxLabels[i];
        if (!lblStatus || !lblBox) continue;

        // ★ 2026-09-09 四态显示（客户现场：物理解锁后仍显示"锁格"）；★ 需求⑦：颜色落在整格底色上
        //   ① 物理锁格中（S7 DB77 锁格位）           → 橙 "锁格"
        //   ② 已物理解锁但 WCS 仍禁用（等 H6 重绑）  → 红 "已解锁·待重绑"
        //   ③ 未禁用且有容器绑定                     → 绿 箱号
        //   ④ 其余                                   → 灰 "未绑定"
        //   优先级由 bpStateOf 统一给出：锁格 > 待重绑 > 已绑定 > 未绑定
        const bool bPhysLocked = m_pPlcMgr && m_pPlcMgr->isGridLocked(gridNum);
        const bool bDisabled   = m_pPlcMgr && m_pPlcMgr->isGridDisabled(gridNum);
        const BindingPanelState state = bpStateOf(boxCode, bPhysLocked, bDisabled);

        if (state == BP_LOCKED)
        {
            boundCount++;
            lockedCount++;   // ★ 2026-09-11：满箱锁格格口计数
            lblStatus->setToolTip(QString("格口%1 物理锁格中（橙色整格）：PLC 已锁定该格口（S7 锁格位置位）").arg(gridKey));
        }
        else if (state == BP_REBIND)
        {
            // ★ 现场已解锁，但满箱后尚未收到 WMS 重发 H6 绑定 → 暂不参与分配（等待重绑）
            boundCount++;
            pendingRebindCount++;
            lblStatus->setToolTip(QString("格口%1 已解锁·待重绑（红色整格）：现场已物理解锁；"
                                          "满箱后旧容器已归档，等待 WMS 重新下发容器绑定(H6)后恢复分配").arg(gridKey));
        }
        else if (state == BP_BOUND)
        {
            boundCount++;
            lblStatus->setToolTip(QString("格口%1 ←→ %2 (已绑定)").arg(gridKey).arg(boxCode));
        }
        else
        {
            lblStatus->setToolTip(QString("格口%1: 未绑定（无底色=默认状态）").arg(gridKey));
        }
        paintGrid(i, state, bpBoxTextOf(state, boxCode));
    }

    // ★ 需求②：未绑定基数 = **可见格口数**（隐藏异常口后为 65；未配置异常口时为 66）
    const int visibleSlots = visibleBindingSlotCount();
    int unboundCount = visibleSlots - boundCount;
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
        m_lblUnboundCount->setToolTip(
            QString::fromUtf8("未绑定: %1（面板可见格口共 %2 个；配置的物理异常口不计入本面板）%3")
                .arg(unboundCount).arg(visibleSlots)
                .arg(pendingRebindCount > 0
                     ? QString::fromUtf8("\n其中 %1 个为「已解锁·待重绑」：等待 WMS 重发 H6 容器绑定")
                           .arg(pendingRebindCount)
                     : QString()));
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

    // ★ 2026-09-15 性能修复：H7/H8 状态计数 + 未闭环异常件数改为**3 条批量查询**覆盖全部波次
    //   原实现"每个波次 3 次同步 DB 查询"，上百波次时主线程数秒冻结（点「切换波次」最明显）。
    //   口径与原逐波次逻辑严格一致（已在 tests/test_wave_records_batch.cpp 用金标准逐条比对锁定）。
    QMap<QString, OutboxStatusCount> h7All, h8All;
    QMap<QString, int>               excAll;
    // ★ 2026-09-17 「格口绑定」列（**默认隐藏**，见 setupUI 的 setColumnHidden）：
    //   该波次绑定过的格口数（= 切回时恢复的格口数）。查询**保留且照常执行** ——
    //   一次 GROUP BY 覆盖全部波次（与上面 3 条批量统计同批），代价可忽略，
    //   换来"把隐藏改成显示"就立刻有数据，不用再改第二处代码。
    QMap<QString, int>               bindCntAll;
    bool batchOk = false;
    if (m_pQueryDb && m_pQueryDb->isOpen())
    {
        bool ok7 = false, ok8 = false, okE = false, okB = false;
        h7All = m_pQueryDb->getFullboxStatusCountAll(&ok7);
        h8All = m_pQueryDb->getEndStatusCountAll(&ok8);
        excAll = m_pQueryDb->getPendingExceptionCountAll(&okE);
        bindCntAll = m_pQueryDb->getBindCountsByOrder(&okB);   // ★ 2026-09-17 一次 GROUP BY 覆盖全部波次
        batchOk = (ok7 && ok8 && okE && okB);
        if (!batchOk)
            appendLog("[波次列表] 批量统计查询失败，本行状态按「无」显示（详见 DataBase 日志）", true);
    }

    for (int row = 0; row < waves.size(); ++row)
    {
        const WaveRecordProgress& w = waves[row];

        // H7 满箱状态汇总（批量结果；无键 = 该波次无报文 → 显示"无"）
        QString h7Status = QString::fromUtf8("无");
        if (h7All.contains(w.orderCode))
        {
            const OutboxStatusCount c = h7All.value(w.orderCode);
            h7Status = QString("成功%1/待发%2/失败%3").arg(c.success).arg(c.pending).arg(c.failed);
        }

        // H8 完结状态汇总（批量结果）
        QString h8Status = QString::fromUtf8("无");
        bool    h8Failed = false;   // ★ 2026-09-08 是否存在失败/已取消的完结报文（状态列标注用）
        if (h8All.contains(w.orderCode))
        {
            const OutboxStatusCount c = h8All.value(w.orderCode);
            h8Status = QString("成功%1/待发%2/失败%3").arg(c.success).arg(c.pending).arg(c.failed);
            h8Failed = (c.failed > 0);
        }

        // 状态列：当前运行波次显示实时状态 + 「（当前）」标记
        // ★ 2026-09-08：处于待执行队列的波次显示「排队待执行」；H8 存在失败报文时追加「（回传失败·待重传）」
        // ★ 2026-09-16 需求①：终态（已完成/已取消）波次追加「（不可切回）」——按需求不允许切回，
        //   报文补发请用「重传满箱切换(H7) / 重传任务完结(H8)」（两者读本表选中行，不依赖切回）
        QString statusText;
        bool inPendingQueue = false;
        for (const HttpServer::PendingWaveInfo& pw : pendingWaves)
        {
            if (pw.orderCode == w.orderCode) { inPendingQueue = true; break; }
        }
        // 终态判定：以内存实时状态为准（当前波次刚完结时 DB 状态可能滞后一拍）
        const int rowStatus = (!liveOrder.isEmpty() && w.orderCode == liveOrder) ? liveStatus : w.status;
        const bool bTerminalRow = (rowStatus == WAVE_FINISHED || rowStatus == WAVE_CANCELLED);
        if (!liveOrder.isEmpty() && w.orderCode == liveOrder)
            statusText = WaveSnapshot::statusToString(liveStatus) + QString::fromUtf8("（当前）");
        else if (inPendingQueue)
            statusText = QString::fromUtf8("已下发（排队待执行）");
        else
            statusText = WaveSnapshot::statusToString(w.status);

        if (h8Failed && w.status != WAVE_FINISHED)
            statusText += QString::fromUtf8("（回传失败·待重传）");
        if (bTerminalRow)
            statusText += QString::fromUtf8("（不可切回）");

        auto setCell = [&](int col, const QString& text) {
            QTableWidgetItem* item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            m_tblWaveRecords->setItem(row, col, item);
            return item;
        };
        // ★ 2026-09-13 客户口径：「处理」= 仍在异常口、尚未处理完的件数（去重 EPC）；
        //   「异常口」= 同一批待处理件（同源同值）——成功落格即视为已处理，两个数字同步递减。
        //   当前波次取内存实时值；历史波次按 DB 计算（已成功落格的 EPC 不再计入）。
        int sortedCnt  = w.sortedCount;
        int excCnt     = w.exceptionCount;     // 历史波次：DB 去重计数（含"未成功落格"的异常 EPC）
        bool needDbCalc = true;
        if (!liveOrder.isEmpty() && w.orderCode == liveOrder)
        {
            if (wm)
            {
                sortedCnt   = wm->sorted();
                excCnt      = wm->exception();
                needDbCalc  = false;   // 当前波次 = 内存实时值
            }
        }
        if (needDbCalc && m_pQueryDb && m_pQueryDb->isOpen())
        {
            // 历史波次：处理/异常口 = exception_record 中 PLC 判定失败两类记录里
            //   **未闭环(handled=0)** 的去重 EPC 数（成功落格即已处理，不再计入）
            //   ★ 2026-09-15：改用批量结果（3 条查询覆盖全部波次），口径与逐波次查询完全一致
            excCnt = excAll.value(w.orderCode, 0);
        }
        setCell(0, w.orderCode);
        setCell(1, statusText);
        {
            QTableWidgetItem* it = setCell(2, QString::number(w.orderQty));
            it->setTextAlignment(Qt::AlignCenter);
            it->setToolTip(QString::fromUtf8("WMS 下发波次计划件数（orderQty）"));
        }
        {
            QTableWidgetItem* it = setCell(3, QString::number(sortedCnt));
            it->setTextAlignment(Qt::AlignCenter);
            it->setToolTip(QString::fromUtf8(
                "已分拣（件次口径）= PLC 落格反馈累计次数（含重复反馈与重投）\n"
                "当前波次=内存实时值；历史波次=sorting_records 去重 EPC 数"));
        }
        {
            QTableWidgetItem* it = setCell(4, QString::number(excCnt));
            it->setTextAlignment(Qt::AlignCenter);
            it->setForeground(excCnt > 0 ? QColor("#D32F2F") : QColor("#333333"));
            it->setToolTip(QString::fromUtf8(
                "处理（件）= 仍在异常口、尚未处理完的件数（去重 EPC）\n"
                "该 EPC 之后成功落格即视为已处理 → 立即减少；不计入「已分拣」\n"
                "当前波次=内存实时值；历史波次=exception_record 中未闭环的 PLC 判定失败 EPC 去重数"));
        }
        {
            QTableWidgetItem* it = setCell(5, QString::number(excCnt));
            it->setTextAlignment(Qt::AlignCenter);
            it->setToolTip(QString::fromUtf8(
                "异常口（件）= 与「处理」同一个量（仍在异常口、尚未处理完的件数，去重 EPC）\n"
                "一个 EPC 只记一次；**成功落格即已处理 → 同步减少**（不再只增不减）\n"
                "需要「本波次曾掉入异常口的总量」请看「异常留痕」或异常明细弹窗"));
        }
        setCell(6, h7Status);
        setCell(7, h8Status);
        setCell(8, formatTimeFirst(w.updatedAt));   // ★ 2026-09-08 时间在前、年月在后
        // ★ 2026-09-17 第 10 列「格口绑定」：**默认隐藏**（现场要求），但列/口径保留 ——
        //   仍照常填充（一次批量查询早已取回，代价可忽略），这样"改一行 false 即可恢复显示"，
        //   恢复后立刻有数据、不会出现空列。文案/配色口径见 tests/BindingPanelPolicy.h。
        if (!m_tblWaveRecords->isColumnHidden(WAVE_RECORDS_BIND_COL))
        {
            const int ownCnt = bindCntAll.value(w.orderCode, 0);
            QTableWidgetItem* it = setCell(WAVE_RECORDS_BIND_COL, bpWaveBindColumnText(ownCnt));
            it->setTextAlignment(Qt::AlignCenter);
            it->setForeground(QColor(bpWaveBindColumnColor(ownCnt)));
            it->setToolTip(bpWaveBindColumnTooltip(ownCnt));
        }

        // ★ 2026-09-16 需求①：终态行整行提示"不可切回"（状态列已标注），并说明正确入口
        if (bTerminalRow)
        {
            for (int c = 0; c < m_tblWaveRecords->columnCount(); ++c)
            {
                if (QTableWidgetItem* cell = m_tblWaveRecords->item(row, c))
                    cell->setToolTip(QString::fromUtf8(
                        "该波次为终态（已完成/已取消）：按需求**不允许切回**。\n"
                        "如需补发失败报文，请选中本行后点「重传满箱切换(H7)」或「重传任务完结(H8)」。"));
            }
        }
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
    //   ③ 下拉为提示项（暂无运行波次/本波次暂无失败记录）→ 沿用原行为（按选中波次/当前波次重发全部未成功 H7）
    //   ★ 2026-09-16 需求⑤：下拉内容已收窄为本波次，故①只可能命中本波次的失败报文
    if (m_cmbFailedH7)
    {
        const QString text = m_cmbFailedH7->currentText().trimmed();
        const bool noFailItem = text.isEmpty()
                             || text == QString::fromUtf8("暂无失败记录")
                             || text == QString::fromUtf8("暂无运行波次")
                             || text == QString::fromUtf8("本波次暂无失败记录");

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
                m_pServer->manualFullbox(grid);   // ★ 需求④：返回值(msgId)此处不需要
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
// ★ 2026-09-16 现场需求④：「一键满箱回传」三项计数的**唯一取数与渲染入口**
//   ① 本波次满箱回传 = 当前波次已生成的 H7 报文总数（outbox_fullbox 行数，含自动满箱/
//      手动满箱/一键回传）+ 成功·待发·失败拆分；数据源 getFullboxStatusCountAll()（已有批量接口）
//   ② 本次一键 成功   = m_oneKeyBatches（本次一键成功生成并入 Outbox 的报文件数）
//   ③ 本次一键 失败   = m_oneKeyFails（未生成报文 + 生成后最终重试耗尽失败）
//   ★ 波次变化时"本次"计数归零、msgId 集合清空（迟到的失败回执不会误计到新波次）
//   ★ 本函数只在"波次变化 / 一键前后 / 失败回执 / 启停接收"时调用，**不每秒查库**
// ============================================================================
void MainWindow::refreshFullboxCountLabel()
{
    if (!m_lblFullboxCount || !m_pServer) return;

    const QString order = (m_pServer->waveManager() ? m_pServer->waveManager()->orderCode() : QString());

    // 波次变化 → "本次一键"归零（口径：本次 = 当前波次内的本次会话一键操作）
    if (order != m_fullboxCountOrder)
    {
        m_fullboxCountOrder = order;
        m_oneKeyBatches      = 0;
        m_oneKeyFails        = 0;
        m_oneKeyMsgIds.clear();
    }

    if (order.isEmpty())
    {
        m_lblFullboxCount->setText(QString::fromUtf8("无运行波次 ｜ 本次一键 成功 0 / 失败 0"));
        m_lblFullboxCount->setStyleSheet("font-size: 12px; color: #888; padding: 0 4px;");
        return;
    }

    int total = 0, succ = 0, pend = 0, fail = 0;
    if (SortingDatabase* db = m_pServer->sortingDb(); db && db->isOpen())
    {
        const OutboxStatusCount c = db->getFullboxStatusCountAll().value(order);
        succ = c.success; pend = c.pending; fail = c.failed; total = c.total();
    }

    QString text = QString::fromUtf8("本波次满箱回传 %1 次（成功%2/待发%3/失败%4） ｜ 本次一键 成功 %5 / 失败 %6")
        .arg(total).arg(succ).arg(pend).arg(fail).arg(m_oneKeyBatches).arg(m_oneKeyFails);
    m_lblFullboxCount->setText(text);
    // 本次一键失败数 > 0 时整条标红，现场一眼可见
    m_lblFullboxCount->setStyleSheet(m_oneKeyFails > 0
        ? "font-size: 12px; color: #D32F2F; font-weight: bold; padding: 0 4px;"
        : "font-size: 12px; color: #555; padding: 0 4px;");
}

// ★ 2026-09-16 需求④：H7 报文最终失败（重试耗尽）回执 → 仅对"本次一键生成的 msgId"累加失败数
void MainWindow::onFullboxMessageFailed(const QString& msgId, const QString& orderCode, const QString& grid)
{
    Q_UNUSED(orderCode);
    Q_UNUSED(grid);
    if (!m_oneKeyMsgIds.contains(msgId)) return;   // 非本次一键来源（自动满箱/手动重传）→ 不计入本次
    m_oneKeyMsgIds.remove(msgId);                  // 已归因，避免后续重复累计
    ++m_oneKeyFails;
    appendLog(QString::fromUtf8("[一键满箱] 其中 1 条报文最终失败（重试耗尽）——本次一键失败数 +1"), true);
    refreshFullboxCountLabel();
}

// ============================================================================
// ★ 2026-09-08 UI需求2/3：刷新「H7 失败格口 / H8 失败波次」两个下拉
//   ★ 2026-09-16 需求⑤：H7 下拉改为**只显示当前运行波次的**失败/已取消报文
//   H8 下拉保持"全部历史失败波次"（否则"已完成波次补发 H8"的入口会断）
//   无记录时显示灰色提示项，并清空可编辑框方便直接手输格口
// ============================================================================
void MainWindow::refreshFailedCombos()
{
    if (!m_pServer) return;

    // ── H7 失败格口下拉（★ 需求⑤：只取当前运行波次）──
    if (m_cmbFailedH7)
    {
        const QString keepText = m_cmbFailedH7->currentText().trimmed();
        const QString curOrder = (m_pServer->waveManager() ? m_pServer->waveManager()->orderCode() : QString());
        {
            QSignalBlocker blocker(m_cmbFailedH7);
            m_cmbFailedH7->clear();
            m_failedH7Items = m_pServer->getFailedFullboxItemsByOrder(curOrder, 200);
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
                // ★ 需求⑤：区分"无运行波次"与"本波次无失败"，避免现场误以为下拉坏了
                m_cmbFailedH7->addItem(curOrder.isEmpty()
                    ? QString::fromUtf8("暂无运行波次")
                    : QString::fromUtf8("本波次暂无失败记录"));
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

// ★ 切换选中波次：未完成→按 DB 进度恢复到内存继续
//   ★ 2026-09-16 现场需求①：**已完成/已取消（终态）波次一律拒绝切回**（不再提供"载入查看"）
//   ★ 2026-09-06 状态隔离：任意非终态状态都可切出；若正在等待 H8 完结，自动取消等待再切换（报文留 outbox 补发）
void MainWindow::onResumeSelectedWave()
{
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

    // ══════════════════════════════════════════════════════════════════════════
    // ★ 2026-09-16 现场需求①：终态波次（已完成/已取消）**不允许切回** —— UI 侧守卫
    //
    //   必须放在**摘要弹窗之前**、且在任何"切出当前波次"的动作之前：
    //     · 一旦放行，HttpServer::resumeUnfinishedWave 的校验1 会先把当前运行波次切出去，
    //       即使随后拒绝载入，当前任务也已经被切出（"拒绝"这个动作产生了副作用）；
    //     · 因此这里先拦，拒绝路径只打日志 + 刷新列表，**什么状态都不改**。
    //   报文补发入口不受影响：用「重传满箱切换(H7) / 重传任务完结(H8)」（读的是波次表选中行）。
    // ══════════════════════════════════════════════════════════════════════════
    if (m_pServer->isWaveTerminal(orderCode))
    {
        const int st = m_pServer->waveDbStatus(orderCode);
        appendLog(QString::fromUtf8(
            "[切换] 波次 %1（%2）已完成/已取消：按需求**不允许切回**（当前运行任务未受影响）；"
            "如需补发报文请用「重传满箱切换(H7) / 重传任务完结(H8)」")
            .arg(orderCode)
            .arg(st > 0 ? WaveSnapshot::statusToString(st) : QString::fromUtf8("终态")), true);
        onRefreshWaveRecords();   // 列表标注同步刷新
        return;
    }

    if (m_stopPhase == StopEnding)
    {
        appendLog("[切换] 正在等待完结回传(H8)——自动取消等待（H8报文保留outbox继续补发），继续切换", true);
        doActualStop();   // 幂等收尾：停止接收 + UI 复位；H8 未确认报文保留
    }

    // 摘要弹窗（含数据快照）
    QJsonObject s = m_pServer->getUnfinishedWaveSummary(orderCode);
    if (s.isEmpty())
    {
        appendLog(QString("[切换] 获取波次摘要失败 order=%1").arg(orderCode), true);
        return;
    }

    int dbStatus = s["status"].toInt();
    // ★ 2026-09-17 格口绑定摘要进入确认弹窗：现场"切回后绑定没回来/被清空"的关键前提是
    //   切回前根本看不到该波次有没有绑定记录 —— 此处先给数字，让操作员自己判断要不要切。
    const int bindOwn      = s.contains("bindOwn")       ? s["bindOwn"].toInt()       : -1;
    const int bindConfirmed = s.contains("bindConfirmed") ? s["bindConfirmed"].toInt() : -1;
    const int bindActive   = s.contains("bindActive")    ? s["bindActive"].toInt()    : -1;
    QString bindLine;
    if (bindOwn < 0)
        bindLine = QString::fromUtf8("格口绑定: （未取得统计）");
    else if (bindOwn == 0)
        bindLine = QString::fromUtf8("格口绑定: **无绑定记录**（切回恢复不出该波次的绑定；库中当前物理绑定 %1 个仅供显示）")
                       .arg(bindActive < 0 ? 0 : bindActive);
    else
        bindLine = QString::fromUtf8("格口绑定: 本波次记录 %1 格（切回将恢复这 %1 个；其中已被更晚波次重新绑定 %2 个）")
                       .arg(bindOwn).arg(qMax(0, bindOwn - qMax(0, bindConfirmed)));

    QString msg = QString(
        "波次号: %1\n"
        "状态: %2\n"
        "计划件数: %3\n"
        "已分拣: %4    异常: %5\n"
        "H7满箱: %6\n"
        "H8完结: %7\n"
        "%8\n"
        "更新时间: %9\n\n"
        "%10")
        .arg(s["orderCode"].toString())
        .arg(s["statusText"].toString())
        .arg(s["orderQty"].toInt())
        .arg(s["sortedCount"].toInt())
        .arg(s["exceptionCount"].toInt())
        .arg(s["h7"].toString())
        .arg(s["h8"].toString())
        .arg(bindLine)
        .arg(s["updatedAt"].toString())
        .arg(QString::fromUtf8("切换到该波次并按其上次进度继续？\n（切换后若当前有其它任务会先保留其进度与数据）"));

    QMessageBox box(QMessageBox::Question, QString::fromUtf8("切换波次"), msg,
                    QMessageBox::Yes | QMessageBox::Cancel, this);
    box.button(QMessageBox::Yes)->setText(QString::fromUtf8("切换到该波次"));
    box.button(QMessageBox::Cancel)->setText(QString::fromUtf8("取消"));
    if (box.exec() != QMessageBox::Yes)
        return;

    appendLog(QString("[切换] 执行切换 order=%1 ...").arg(orderCode));
    bool ok = m_pServer->resumeUnfinishedWave(orderCode);
    if (ok)
    {
        int status = m_pServer->waveManager() ? m_pServer->waveManager()->status() : -1;
        // ★ 2026-09-17 切回后回报"格口绑定到底恢复成什么样"（来源+数量），
        //   不再让"切过去发现绑定没恢复"只能靠翻日志才知道。
        const int restoredCnt = m_pServer->boundCount();
        const QString bindResult = (bindOwn > 0)
            ? QString::fromUtf8("格口绑定：已恢复 %1 个（来源：本波次记录）").arg(restoredCnt)
            : ((bindActive > 0)
                ? QString::fromUtf8("格口绑定：**该波次无绑定记录**，已按现场物理绑定显示 %1 个（非本波次记录，等 WMS 重发 H6）").arg(restoredCnt)
                : QString::fromUtf8("格口绑定：无任何绑定可恢复（该波次无记录且现场无活跃绑定）——请等 WMS 下发 H6"));
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
                QString::fromUtf8("已切换波次：%1\n%2\n%3").arg(orderCode).arg(bindResult).arg(hint));
        }
        else
        {
            // ★ 2026-09-16 需求①：终态波次已不允许切回（入口守卫），此处仅作兜底提示
            QMessageBox::information(this, QString::fromUtf8("已切换"),
                QString::fromUtf8("已切换波次：%1（当前状态 %2）\n%3")
                    .arg(orderCode).arg(WaveSnapshot::statusToString(status)).arg(bindResult));
        }
        updateWavePanel();
        updateBindingPanel();
        onRefreshWaveRecords();
    }
    else
    {
        appendLog(QString("[切换] 切换失败 order=%1（详见上方原因）").arg(orderCode), true);
        onRefreshWaveRecords();   // ★ 需求①：被拒绝的终态行需同步刷新列表标注
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
                    "  ② **清空格口容器绑定**：面板全部格口复位为「未绑定」（② 的绑定记录归档保留在数据库中，\n"
                    "     切回该波次时会逐格自动恢复，不会丢失）；\n"
                    "  ③ 界面回到空闲、格口状态复位为初始全新状态，等待 WMS 下发新波次（届时由 WMS 重新下发 H6 绑定）；\n"
                    "  ④ 未成功的 H7/H8 回传可在切回该波次时自动补发，或用「重传」按钮。\n\n"
                    "确定开始新任务吗？")
                .arg(curOrder).arg(WaveSnapshot::statusToString(curStatus)),
            QMessageBox::Yes | QMessageBox::Cancel);
        if (ret != QMessageBox::Yes)
            return;
    appendLog(QString("[新任务] 开始新任务，当前波次进度已保留 order=%1（%2）；格口容器绑定将一并清空"
                      "（面板全部「未绑定」，记录归档保留、切回该波次时自动恢复）")
        .arg(curOrder).arg(WaveSnapshot::statusToString(curStatus)));
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

// ============================================================================
// ★ 2026-09-16 需求④：按「效率统计」按钮的勾选状态显隐「运行日志」页右侧的效率面板
//   · on=true  → 首次调用时才**懒创建**面板（那时 HttpServer 已就绪，数据即刻可用），
//                并立即刷新一次（不等 1 秒定时器，点开就有数）
//   · on=false → 隐藏面板（与其承载容器一起），日志区自动吃满宽度
//   调用点：①「效率统计」按钮 toggled 信号；② 构造函数在 setupCore() 后按默认勾选状态应用一次
// ============================================================================
void MainWindow::applyLogEffPanelVisible(bool on)
{
    if (!m_logEffHost) return;   // 日志页尚未构建（构造期防御）

    if (on && !m_logEffPanel)
    {
        auto* panel = new LogEfficiencyPanel(m_pServer, m_logEffHost);
        panel->setMinimumWidth(280);
        if (auto* hostLay = qobject_cast<QVBoxLayout*>(m_logEffHost->layout()))
            hostLay->addWidget(panel);
        m_logEffPanel = panel;
        m_logEffTick  = [this]() {
            if (m_logEffPanel) static_cast<LogEfficiencyPanel*>(m_logEffPanel)->tick();
        };
        // ★ 2026-09-17 现场要求：面板宽度 = 「波次信息」面板宽度（上下两栏对齐），
        //   （原为"日志区与面板各占一半"；现改为跟随波次信息面板的实际宽度，随窗口缩放同步）
        m_logEffSyncedW = -1;          // 强制本次按当前宽度对齐一次
        syncLogEffPanelWidth();
    }

    m_logEffHost->setVisible(on);
    if (on && m_logEffTick) m_logEffTick();          // 立即出数
    if (on) syncLogEffPanelWidth();                  // 显示时再对齐一次（首次创建时宽度可能尚未确定）
}

// ============================================================================
// ★ 2026-09-17 现场要求：效率统计面板宽度 = 「波次信息」面板宽度
//   · 波次信息面板在第一行右侧（占整行一半，可拖动分隔条改变）；
//   · 效率统计面板在「运行日志」页右侧 → 两者同宽即可上下对齐，视觉成列。
//   · 实现：把日志页分隔条的右栏宽度设为波次信息面板的当前宽度（左栏吃余量）。
//   · 频率：由 1 秒刷新定时器调用，但**记住上次对齐值**，宽度未变时直接返回（零布局开销）；
//     用户手动拖动日志分隔条后不被秒级覆盖，只有窗口尺寸/波次信息面板宽度变化时才重新对齐。
// ============================================================================
void MainWindow::syncLogEffPanelWidth()
{
    if (!m_logEffSplit || !m_logEffHost || !m_logEffPanel || !m_grpWaveInfo) return;
    if (!m_logEffHost->isVisible()) return;                 // 面板隐藏/不在前台：不动分隔条
    const int target = m_grpWaveInfo->width();
    const int total  = m_logEffSplit->width();
    if (target <= 0 || total <= 0 || target >= total) return;   // 几何未就绪：下次再对
    if (target == m_logEffSyncedW) return;                      // 已对齐过且宽度未变 → 直接返回

    QList<int> sizes;
    sizes << (total - target) << target;
    m_logEffSplit->setSizes(sizes);
    m_logEffSyncedW = target;
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
// ★ 2026-09-13 onViewExceptions — 打开「异常明细」弹窗（波次信息「异常」右侧按钮）
//   默认波次：当前内存波次；若列表里选中了波次则以选中为准（便于回看历史波次异常）
//   单实例复用：已打开则置前；关闭即销毁（WA_DeleteOnClose）
// ============================================================================
void MainWindow::onViewExceptions()
{
    if (!m_pQueryDb) return;

    if (!m_pQueryDb->isOpen())
    {
        const QString dbPath = QCoreApplication::applicationDirPath() + "/" + SORTING_DB_FILE;
        if (!m_pQueryDb->open(dbPath))
        {
            appendLog("[异常] 数据库未就绪，无法查看异常明细", true);
            return;
        }
    }

    if (m_excDlg)
    {
        m_excDlg->show();
        m_excDlg->raise();
        m_excDlg->activateWindow();
        return;
    }

    // 目标波次：列表选中行优先 → 当前内存波次
    QString order = selectedOrCurrentWaveOrder();
    if (order.isEmpty() && m_pServer && m_pServer->waveManager())
        order = m_pServer->waveManager()->orderCode();

    ExceptionListDialog* dlg = new ExceptionListDialog(m_pQueryDb, order, this);
    dlg->setContext(m_pServer, [this](const QString& text) { appendLog(text); });
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_excDlg = dlg;
    connect(dlg, &QDialog::destroyed, this, [this]() { m_excDlg = nullptr; });
    dlg->show();
    appendLog(QString::fromUtf8("[异常] 打开异常明细：波次=%1")
        .arg(order.isEmpty() ? QString::fromUtf8("(全部)") : order));
}

// ============================================================================
// ★ 2026-09-14 独立窗口页「计划分配表」
//   客户要求：**不在波次信息面板里加内容**，单独一页展示该波次的计划分配。
//   口径：一行 = 一个产品（SKU），横向按格口展开；每个格口一组
//         「计划 / 已落 / 在途 / 余量」；类型只区分「正常分拣 / 发货」。
//
//   性能与主线程安全（日万级件、单波次可达 5 万件）：
//     · 每 2 秒只读一个版本号（HttpServer 内的原子 int）；版本未变、
//       或本页不在前台 → 直接返回，**不取快照、不碰表格**（空闲开销 ≈ 0）；
//     · 版本变化才做一次快照（锁内拷内存，不查 DB），且只重填**当前可见页**；
//     · 每页固定 100 行（复用 SORTING_QUERY_PAGE_SIZE），格口组最多 12 组
//       → 重建成本恒定，与波次件数无关；
//     · 全程只读，不调用任何写接口，避免误触改账。
// ============================================================================
void MainWindow::refreshPlanAllocPage(bool force)
{
    if (!m_tblPlanAlloc || !m_pServer) return;

    // ① 本页不在前台 → 不刷新（用户看不到，重建纯属浪费主线程时间）
    if (!force && m_tabMain && m_pagePlanAlloc && m_tabMain->currentWidget() != m_pagePlanAlloc)
        return;

    // ② 2 秒节流（仅对"非强制"的周期刷新生效；切页 force=true 时立即刷新）
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!force && nowMs - m_planAllocLastRefreshMs < 2000) return;
    m_planAllocLastRefreshMs = nowMs;

    PlanAllocSnapshot snap = m_pServer->planAllocSnapshot();

    // ③ 波次/表版本都没变 → 无需重建（数量变化会 bump version）
    //    force=true 时同时比较版本：只有确实变化才重建，避免切页空跑一遍重绘
    if (snap.version == m_planAllocVersion && m_planAllocSnap.orderCode == snap.orderCode)
        return;
    m_planAllocVersion  = snap.version;
    m_planAllocSnap     = snap;
    m_planAllocPage     = 1;
    if (m_spinPlanAllocPage) m_spinPlanAllocPage->setValue(1);

    // ④ 汇总条
    if (m_lblPlanAllocSum)
    {
        if (snap.orderCode.isEmpty())
        {
            m_lblPlanAllocSum->setText(QString::fromUtf8("当前无运行波次"));
            m_lblPlanAllocSum->setStyleSheet("font-size: 13px; font-weight: bold; color: #78909C;");
        }
        else
        {
            const QString txt = QString::fromUtf8(
                "波次 %1 ｜ 产品 %2 个（多格口 %3）｜ 计划件数 %4 ｜ 已落 %5 ｜ 在途 %6 ｜ 余量 %7 ｜ 预警 %8%9")
                .arg(snap.orderCode).arg(snap.skuCount).arg(snap.multiSkuCnt)
                .arg(snap.planTotal).arg(snap.landedTotal).arg(snap.reservTotal).arg(snap.remainTotal)
                .arg(snap.warnCount)
                .arg(!snap.valid ? QString::fromUtf8(" ｜ ⚠分配表未生效(按原逻辑选格)")
                                 : (snap.auditBad > 0 ? QString::fromUtf8(" ｜ ⚠不变量违规 %1 处").arg(snap.auditBad)
                                                      : QString()));
            m_lblPlanAllocSum->setText(txt);
            m_lblPlanAllocSum->setStyleSheet(snap.auditBad > 0 || snap.warnCount > 0
                ? "font-size: 13px; font-weight: bold; color: #D32F2F;"
                : "font-size: 13px; font-weight: bold; color: #37474F;");
        }
    }

    renderPlanAllocRows();
}

// 按当前页/过滤重填表格（不重新取快照）
void MainWindow::renderPlanAllocRows()
{
    if (!m_tblPlanAlloc) return;

    const PlanAllocSnapshot& snap = m_planAllocSnap;

    // ── 过滤（产品/SKU）──
    QVector<const PlanAllocRow*> rows;
    rows.reserve(snap.rows.size());
    for (const PlanAllocRow& r : snap.rows)
    {
        if (!m_planAllocFilter.isEmpty() &&
            !r.sku.contains(m_planAllocFilter, Qt::CaseInsensitive))
            continue;
        rows.append(&r);
    }

    // ── 格口列（组）── 上限 12 组，避免横向无限拉宽
    const int kMaxGridGroups = 12;
    QStringList gridCols = snap.gridColumns;
    const bool truncated = gridCols.size() > kMaxGridGroups;
    if (truncated) gridCols = gridCols.mid(0, kMaxGridGroups);

    const int kFixedCols = 8;                     // 序号/产品/合计4/格口数/格口明细
    const int pageSize   = SORTING_QUERY_PAGE_SIZE;  // 100 行/页（与查询页一致）
    const int totalRows  = rows.size();
    const int totalPages = qMax(1, (totalRows + pageSize - 1) / pageSize);
    if (m_planAllocPage < 1) m_planAllocPage = 1;
    if (m_planAllocPage > totalPages) m_planAllocPage = totalPages;
    if (m_spinPlanAllocPage)
    {
        const QSignalBlocker blk(m_spinPlanAllocPage);
        m_spinPlanAllocPage->setRange(1, totalPages);
        m_spinPlanAllocPage->setValue(m_planAllocPage);
    }

    const int from = (m_planAllocPage - 1) * pageSize;
    const int to   = qMin(totalRows, from + pageSize);

    // ── 复用列：列数变化时才 setColumnCount（避免每次重建整个表头）──
    const int needCols = kFixedCols + gridCols.size() * 4;
    {
        QSignalBlocker blk(m_tblPlanAlloc);
        m_tblPlanAlloc->setUpdatesEnabled(false);
        if (m_tblPlanAlloc->columnCount() != needCols)
        {
            m_tblPlanAlloc->clear();
            m_tblPlanAlloc->setColumnCount(needCols);
        }
        m_tblPlanAlloc->setRowCount(0);

        QStringList heads;
        heads << QString::fromUtf8("序号") << QString::fromUtf8("产品编码(SKU)")
              << QString::fromUtf8("计划总件数") << QString::fromUtf8("已落格")
              << QString::fromUtf8("在途") << QString::fromUtf8("余量")
              << QString::fromUtf8("格口数") << QString::fromUtf8("格口明细（格口:类型=计划/已落/余量）");
        for (const QString& gk : gridCols)
        {
            // 组标题：034(正常分拣) → 组内 4 列 计划/已落/在途/余量
            QString typeName = QString::fromUtf8("格口");
            for (const PlanAllocRow& r : snap.rows)
                for (const PlanAllocCell& c : r.cells)
                    if (c.gridKey == gk)
                    {
                        typeName = (c.gridType == "2") ? QString::fromUtf8("发货")
                                 : (c.gridType == "1") ? QString::fromUtf8("异常")
                                                       : QString::fromUtf8("正常分拣");
                        break;
                    }
            heads << QString("%1(%2)").arg(gk).arg(typeName)
                  << QString::fromUtf8("计划") << QString::fromUtf8("已落")
                  << QString::fromUtf8("在途") << QString::fromUtf8("余量");
        }
        m_tblPlanAlloc->setHorizontalHeaderLabels(heads);

        // 列宽：固定列窄、格口组内 4 列紧凑
        m_tblPlanAlloc->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        m_tblPlanAlloc->setColumnWidth(0, 48);
        m_tblPlanAlloc->setColumnWidth(1, 170);
        for (int c = 2; c < kFixedCols; ++c) m_tblPlanAlloc->setColumnWidth(c, 72);
        m_tblPlanAlloc->setColumnWidth(7, 300);   // 格口明细列宽一些（可复制核对）
        for (int g = 0; g < gridCols.size(); ++g)
        {
            const int base = kFixedCols + g * 4;
            m_tblPlanAlloc->setColumnWidth(base, 92);      // 只有标题的列占位（组标题在左格）
            for (int k = 1; k < 4; ++k) m_tblPlanAlloc->setColumnWidth(base + k, 52);
        }

        m_tblPlanAlloc->setRowCount(to - from);
        for (int i = from; i < to; ++i)
        {
            const PlanAllocRow& r = *rows[i];
            const int row = i - from;
            auto setCell = [&](int col, const QString& txt, const QColor& fg = QColor("#333333"),
                               const QColor& bg = QColor()) {
                QTableWidgetItem* it = new QTableWidgetItem(txt);
                it->setTextAlignment(Qt::AlignCenter);
                it->setForeground(fg);
                if (bg.isValid()) it->setBackground(bg);
                m_tblPlanAlloc->setItem(row, col, it);
            };

            setCell(0, QString::number(i + 1));
            setCell(1, r.sku);
            setCell(2, QString::number(r.planTotal));
            setCell(3, QString::number(r.landedTotal),
                    r.landedTotal >= r.planTotal && r.planTotal > 0 ? QColor("#D32F2F") : QColor("#333333"));
            setCell(4, QString::number(r.reservTotal),
                    r.reservTotal > 0 ? QColor("#0277BD") : QColor("#333333"));
            setCell(5, QString::number(r.remainTotal),
                    r.remainTotal > 0 ? QColor("#2E7D32") : QColor("#D32F2F"));
            setCell(6, QString::number(r.cells.size()),
                    r.cells.size() > 1 ? QColor("#6A1B9A") : QColor("#333333"));

            // 格口明细：始终列**全部**格口（不受 12 组上限影响），便于复制/grep 核对
            QStringList detail;
            for (const PlanAllocCell& c : r.cells)
            {
                const QString tn = (c.gridType == "2") ? QString::fromUtf8("发货")
                                 : (c.gridType == "1") ? QString::fromUtf8("异常")
                                                       : QString::fromUtf8("正常分拣");
                detail << QString("%1:%2=%3/%4/%5").arg(c.gridKey).arg(tn)
                              .arg(c.planQty).arg(c.landedQty).arg(c.remainQty);
            }
            setCell(7, detail.join(QString::fromUtf8("  ")));

            // 每个格口组 4 列：计划/已落/在途/余量（不在计划内 → "—"）
            for (int g = 0; g < gridCols.size(); ++g)
            {
                const int base = kFixedCols + g * 4;
                const PlanAllocCell* cell = nullptr;
                for (const PlanAllocCell& c : r.cells)
                    if (c.gridKey == gridCols[g]) { cell = &c; break; }

                if (!cell)
                {
                    for (int k = 0; k < 4; ++k) setCell(base + k, QString::fromUtf8("—"), QColor("#BDBDBD"));
                    continue;
                }
                // 组标题列显示格口状态（容器/禁用/锁格），一眼判断"能不能落"
                QString stateTxt;
                QColor  stateBg, stateFg;
                if (cell->disabled)      { stateTxt = QString::fromUtf8("满箱未重绑"); stateBg = QColor("#FFCDD2"); stateFg = QColor("#B71C1C"); }
                else if (cell->locked)   { stateTxt = QString::fromUtf8("锁格");       stateBg = QColor("#FFE0B2"); stateFg = QColor("#E65100"); }
                else if (!cell->bound)   { stateTxt = QString::fromUtf8("未绑定容器"); stateBg = QColor("#FFF9C4"); stateFg = QColor("#F57F17"); }
                else                     { stateTxt = cell->boxcode;                  stateFg = QColor("#455A64"); }
                QTableWidgetItem* gItem = new QTableWidgetItem(stateTxt);
                gItem->setTextAlignment(Qt::AlignCenter);
                gItem->setForeground(stateFg);
                if (stateBg.isValid()) gItem->setBackground(stateBg);
                gItem->setToolTip(QString("%1(%2) 容器%3%4%5")
                    .arg(cell->gridKey)
                    .arg((cell->gridType == "2") ? QString::fromUtf8("发货")
                         : (cell->gridType == "1") ? QString::fromUtf8("异常") : QString::fromUtf8("正常分拣"))
                    .arg(cell->boxcode.isEmpty() ? QString::fromUtf8("(无)") : cell->boxcode)
                    .arg(cell->disabled ? QString::fromUtf8(" ｜ 满箱未重绑(禁用)") : QString())
                    .arg(cell->locked ? QString::fromUtf8(" ｜ 物理锁格") : QString()));
                m_tblPlanAlloc->setItem(row, base, gItem);

                setCell(base + 1, QString::number(cell->planQty));
                setCell(base + 2, QString::number(cell->landedQty),
                        cell->planQty > 0 && cell->landedQty >= cell->planQty ? QColor("#D32F2F") : QColor("#333333"));
                setCell(base + 3, QString::number(cell->reservQty),
                        cell->reservQty > 0 ? QColor("#0277BD") : QColor("#333333"));
                setCell(base + 4, QString::number(cell->remainQty),
                        cell->remainQty > 0 ? QColor("#2E7D32") : QColor("#D32F2F"));
            }
        }
        m_tblPlanAlloc->setUpdatesEnabled(true);
    }

    // ⑤ 页脚提示（分页/截断说明）
    if (m_lblPlanAllocHint)
    {
        QStringList h;
        h << QString::fromUtf8("共 %1 个产品%2，第 %3/%4 页（每页 %5 行）")
                 .arg(totalRows)
                 .arg(m_planAllocFilter.isEmpty() ? QString()
                                                  : QString::fromUtf8("（过滤：%1）").arg(m_planAllocFilter))
                 .arg(m_planAllocPage).arg(totalPages).arg(pageSize);
        h << QString::fromUtf8("格口列显示 %1 个").arg(gridCols.size());
        if (truncated)
            h << QString::fromUtf8("格口超过 12 个，其余请在「格口明细」列查看（该列不截断）");
        h << QString::fromUtf8("列头颜色：黄=未绑定容器 红=满箱未重绑 橙=锁格；" 
                               "「已落」红色=已达计划，「余量」绿色=仍可落");
        m_lblPlanAllocHint->setText(h.join(QString::fromUtf8(" ｜ ")));
    }
}

// ============================================================================
// ★ 2026-09-13 超计划预警（波次面板「预警」数字 + 「查看」明细弹窗）
//   数据源：HttpServer 的 格口+SKU 真实落格计数（PLC 确认落格的去重 EPC）与计划件数对照
//   成因：同一 SKU 有两件同时在线上、或人工多放 → 箱内实落超过计划
//   处置：只告警不改上传数据（乙方案下 H7 报文已按计划件数裁剪，不会因超报被整条驳回）
// ============================================================================
void MainWindow::refreshOverplanWarning()
{
    if (!m_lblOverplanWarn || !m_btnOverplanView) return;

    int n = 0;
    if (m_pServer)
        n = m_pServer->overplanWarningCount();

    m_lblOverplanWarn->setText(QString("%1 条").arg(n));
    m_lblOverplanWarn->setStyleSheet(n > 0
        ? "font-size: 15px; font-weight: bold; color: #D32F2F;"
        : "font-size: 15px; font-weight: bold; color: #2196F3;");
    m_btnOverplanView->setEnabled(n > 0);
    m_btnOverplanView->setText(n > 0
        ? QString::fromUtf8("查看(%1)").arg(n)
        : QString::fromUtf8("查看"));
}

void MainWindow::showOverplanWarningDialog()
{
    if (!m_pServer) return;

    const QVector<HttpServer::OverplanWarning> warns = m_pServer->overplanWarnings();

    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8("超计划预警明细"));
    dlg.resize(980, 460);
    QVBoxLayout* lay = new QVBoxLayout(&dlg);

    // ── 顶部说明 ──
    QLabel* tip = new QLabel(QString::fromUtf8(
        "口径：按「格口 + SKU」比较 本格口计划件数 与 PLC 确认真正落入该格口的去重件数（跨容器累计）。\n"
        "★ 每个格口各有自己的产品计划数（正常分拣/分类口 与 发货口 分别一份计划），故：\n"
        "   「本格口计划」= 该 SKU 在这个格口的计划件数（分配与封顶都以它为准）；\n"
        "   「SKU总计划」= 该 SKU 各格口计划之和，仅作参考；「类型」用于区分分类口/发货口/异常口。\n"
        "超计划成因：同一 SKU 有两件同时在线上（都未落格），或人工多放了一件；改投异常口的件不计入本表。\n"
        "处置：多余件不进入上传报文（H7 已按本格口计划件数裁剪），但实物可能已在箱内 → 请按下列清单现场取出。"));
    tip->setWordWrap(true);
    tip->setStyleSheet("font-size: 13px; color: #555;");
    lay->addWidget(tip);

    // ── 明细表 ──
    QTableWidget* tbl = new QTableWidget();
    tbl->setColumnCount(8);
    tbl->setHorizontalHeaderLabels({
        QString::fromUtf8("格口号"),
        QString::fromUtf8("类型"),
        QString::fromUtf8("容器号"),
        QString::fromUtf8("SKU编码"),
        QString::fromUtf8("本格口计划"),
        QString::fromUtf8("SKU总计划"),
        QString::fromUtf8("实际落格件数"),
        QString::fromUtf8("多余件数")
    });
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    tbl->setSelectionMode(QAbstractItemView::SingleSelection);
    tbl->setFont(QFont(font().family(), 13));
    tbl->verticalHeader()->setDefaultSectionSize(30);
    tbl->horizontalHeader()->setStretchLastSection(true);
    tbl->setStyleSheet(
        "QTableWidget { font-size: 13px; }"
        "QTableWidget::item { padding: 3px 6px; }"
        "QHeaderView::section { background-color: #FFE0B2; font-weight: bold; padding: 6px; }");

    tbl->setRowCount(warns.size());
    for (int i = 0; i < warns.size(); ++i)
    {
        const HttpServer::OverplanWarning& w = warns[i];

        // 容器号：取该格口当前绑定（与"落格时固化"口径可能不同，仅作现场定位参考）
        QString box;
        if (m_pServer && m_pServer->waveManager())
        {
            // 通过 HttpServer 的绑定查询接口取当前绑定（无接口时留空，不阻塞弹窗）
            box = m_pServer->currentBoxOfGrid(w.gridKey);
        }

        auto setCell = [&](int col, const QString& txt, const QColor& c = QColor("#333333")) {
            QTableWidgetItem* it = new QTableWidgetItem(txt);
            it->setTextAlignment(Qt::AlignCenter);
            it->setForeground(c);
            tbl->setItem(i, col, it);
        };
        // 格口类型显示名（0=分类/正常分拣, 1=异常, 2=发货）
        QString typeText = w.gridType;
        if (w.gridType == "0")      typeText = QString::fromUtf8("分类");
        else if (w.gridType == "1") typeText = QString::fromUtf8("异常");
        else if (w.gridType == "2") typeText = QString::fromUtf8("发货");
        else                        typeText = QString::fromUtf8("—");

        setCell(0, w.gridKey);
        setCell(1, typeText);
        setCell(2, box.isEmpty() ? QString::fromUtf8("—") : box);
        setCell(3, w.sku);
        setCell(4, QString::number(w.planQty));
        setCell(5, QString::number(w.skuPlanQty));
        setCell(6, QString::number(w.landedQty));
        setCell(7, QString::number(w.overQty), QColor("#D32F2F"));

        // 多余件 EPC 清单放进 SKU 单元格的 tooltip（列宽有限，不占表格空间）
        if (!w.epcs.isEmpty() && tbl->item(i, 3))
        {
            tbl->item(i, 3)->setToolTip(QString::fromUtf8("多余件 EPC 清单（%1 件）：\n%2")
                .arg(w.epcs.size()).arg(w.epcs.join("\n")));
        }
    }
    lay->addWidget(tbl);

    // ── 多余件 EPC 汇总（便于一次性抄下来去现场找件）──
    QStringList allEpcs;
    for (const HttpServer::OverplanWarning& w : warns)
        for (const QString& e : w.epcs)
            allEpcs << QString::fromUtf8("%1（格口%2 / SKU %3）").arg(e).arg(w.gridKey).arg(w.sku);
    QLabel* epcLbl = new QLabel(allEpcs.isEmpty()
        ? QString::fromUtf8("无多余件 EPC")
        : QString::fromUtf8("多余件 EPC 清单：\n") + allEpcs.join("\n"));
    epcLbl->setWordWrap(true);
    epcLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    epcLbl->setStyleSheet("font-family: Consolas,'Microsoft YaHei'; font-size: 12px; color: #D32F2F;");
    lay->addWidget(epcLbl);

    QDialogButtonBox* btns = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(btns);

    if (warns.isEmpty())
        appendLog("[预警] 当前无超计划预警（实际落格件数均未超过计划件数）");
    else
        appendLog(QString::fromUtf8("[预警] 超计划条目 %1 个，多余件合计 %2 件——请现场从对应容器取出")
            .arg(warns.size())
            .arg([&]() { int s = 0; for (const auto& w : warns) s += w.overQty; return s; }()), true);

    dlg.exec();
}

// ============================================================================
// ★ 2026-09-13 需求：按容器号查询
//   显示该容器号下**所有 EPC 物件**及其相关信息：
//     波次号 / EPC编码 / SKU编码 / 实际落格号(格口) / WMS格口编码 / 计划格口 /
//     格口类型 / 计划数量 / 容器号 / 落格时间 / 状态 / 备注
//   口径说明（现场核对用）：
//     · 落格明细来源 sorting_records.boxcode（落格那一刻的格口绑定），按落格时间正序
//     · 计划来源 return_wave_item（按 波次+SKU 关联）——只用于判断"计划外落格 / 超计划"，
//       ★ 不做计划上限拦截（客户明确要求：WCS 只如实记录，超不超由 WMS 计划侧把握）
//     · 备注列自动标出三类可疑情况：同EPC多容器 / 同SKU超计划 / 计划外落格
//   ★ 末尾附「同格口逐箱对照」，便于定位跨容器漂移（本次现场 034 格口五箱事件即此症状）
// ============================================================================
void MainWindow::renderContainerQuery()
{
    SortingDatabase* db = m_pQueryDb;
    const QString box = m_editQueryBarcode->text().trimmed();

    if (box.isEmpty())
    {
        appendLog("[查询] 请输入容器号（如 H-T0129）", true);
        return;
    }

    // ★ 2026-09-16 需求③：按容器号查询同样受"日期区间（必填）"约束
    const QDateTime qFrom(m_editQueryDateFrom->date(), QTime(0, 0, 0));
    const QDateTime qTo(m_editQueryDateTo->date(), QTime(23, 59, 59));
    const QString   rangeText = QString::fromUtf8("%1 ~ %2")
        .arg(m_editQueryDateFrom->date().toString("yyyy-MM-dd"))
        .arg(m_editQueryDateTo->date().toString("yyyy-MM-dd"));

    QVector<SortingRecord> recs = db->queryByBoxcode(box, qFrom, qTo, SORTING_QUERY_MAX_RESULTS);

    // ── ① 计划信息缓存：波次+SKU → 计划件数 / 计划格口 / 格口类型 / 库位 ──
    //   ★ 按 (波次,SKU) 缓存，避免逐行查库（一个容器可能有上百个 SKU）
    struct PlanInfo
    {
        int         qty = 0;
        QStringList grids;
        QString     typeText;
        QString     volu;
    };
    QMap<QString, PlanInfo> planCache;               // key = orderCode + "\n" + sku
    QMap<QString, int>      planQtyByWaveSku;

    auto planOf = [&](const QString& wave, const QString& sku) -> const PlanInfo& {
        const QString key = wave + "\n" + sku;
        auto it = planCache.find(key);
        if (it != planCache.end()) return it.value();

        PlanInfo pi;
        const QVector<ReturnWaveItemRecord> items = db->querySkuGridMapping(sku, wave);
        for (const ReturnWaveItemRecord& itm : items)
        {
            pi.qty += itm.planQty;
            const QString g = gridKeyOf(itm.gridNum);
            if (!g.isEmpty() && !pi.grids.contains(g)) pi.grids << g;
            if (pi.typeText.isEmpty())
            {
                if (itm.gridType == "0")      pi.typeText = QString::fromUtf8("分类");
                else if (itm.gridType == "1") pi.typeText = QString::fromUtf8("异常");
                else if (itm.gridType == "2") pi.typeText = QString::fromUtf8("发货");
                else                          pi.typeText = itm.gridType;
            }
            if (pi.volu.isEmpty() && !itm.volu.isEmpty()) pi.volu = itm.volu;
        }
        planQtyByWaveSku[key] = pi.qty;
        return planCache.insert(key, pi).value();
    };

    // ── ② 汇总口径：EPC 去重件数 / SKU / 波次 / 格口 ──
    QMap<QString, int>     epcRows;        // EPC → 本容器落格条数（>1 = 重复反馈/重投）
    QMap<QString, QSet<QString>> skuEpcs;  // SKU → 本容器去重 EPC 集合
    QSet<QString> uniqEpcs, uniqSkus, uniqWaves, gridsInBox;
    for (const SortingRecord& r : recs)
    {
        epcRows[r.barcode]++;
        skuEpcs[r.sku].insert(r.barcode);
        uniqEpcs.insert(r.barcode);
        uniqSkus.insert(r.sku);
        uniqWaves.insert(r.orderCode);
        gridsInBox.insert(gridKeyOf(r.gridNum));
    }

    // ── ③ 跨容器检测：只查本容器时看不出某件是否也落到过别的容器，需按 EPC 批量反查 ──
    QStringList epcList;
    for (const QString& e : uniqEpcs) if (!e.isEmpty()) epcList << e;
    const QMap<QString, QStringList> otherBoxes = db->queryOtherBoxcodesByEpc(epcList, box, qFrom, qTo);

    // ── ④ 组装行 ──
    struct BoxRow
    {
        QString wave, epc, sku, grid, wmsCode, planGrid, gridType, planQty, box, time, note;
        bool planOutside = false;    // 实际落格格口不在该 SKU 的计划格口内
        bool skuOver     = false;    // 该 SKU 在本容器的去重件数 > 计划件数（SKU 级）
        bool skuOverflowRow = false; // ★ 本行是"超出计划"的那一件（件级）
        int  overflowSeq = 0;        // 第几件超出计划
        bool drift       = false;    // 同一 EPC 出现在多个容器
        bool dupRow      = false;    // 同一 EPC 在本容器有多条明细
    };
    QVector<BoxRow> rows;
    rows.reserve(recs.size());
    QMap<QString, QSet<QString>> seenEpcsOfSku;   // 波次+SKU → 已累计的去重 EPC（定位"第几件超计划"）
    int overflowRowCount = 0;                     // ★ 超计划件数（件级，供汇总标签显示）
    for (const SortingRecord& r : recs)
    {
        BoxRow b;
        b.wave    = r.orderCode;
        b.epc     = r.barcode;
        b.sku     = r.sku;
        b.grid    = gridKeyOf(r.gridNum);
        b.wmsCode = gridToWmsCode(b.grid);
        b.box     = r.boxcode;
        b.time    = r.sortTime;

        const PlanInfo& pi = planOf(r.orderCode, r.sku);
        b.planQty  = QString::number(pi.qty);
        b.gridType = pi.typeText;
        b.planGrid = pi.grids.isEmpty() ? QString::fromUtf8("(无计划)") : pi.grids.join("/");

        // ★ 超计划要精确到"件"：按落格先后累计该 SKU 的去重 EPC，
        //   第 (计划数+1) 件起才标"超出计划"，而不是把该 SKU 的所有行都标红
        QSet<QString>& seen = seenEpcsOfSku[r.orderCode + "\n" + r.sku];
        const bool isNewEpc = !seen.contains(r.barcode);
        if (isNewEpc) seen.insert(r.barcode);
        if (pi.qty > 0 && isNewEpc && seen.size() > pi.qty)
        {
            b.skuOverflowRow = true;
            b.overflowSeq    = seen.size() - pi.qty;
            ++overflowRowCount;
        }
        b.skuOver     = (pi.qty > 0) && (skuEpcs.value(r.sku).size() > pi.qty);
        b.planOutside = pi.qty > 0 && !pi.grids.isEmpty() && !pi.grids.contains(b.grid);
        b.drift       = otherBoxes.contains(r.barcode);
        b.dupRow      = epcRows.value(r.barcode) > 1;

        QStringList notes;
        if (b.drift)   notes << QString::fromUtf8("同EPC多容器[%1]").arg(otherBoxes.value(r.barcode).join(","));
        if (b.dupRow)  notes << QString::fromUtf8("本容器重复%1条").arg(epcRows.value(r.barcode));
        if (b.skuOverflowRow) notes << QString::fromUtf8("★超出计划第%1件").arg(b.overflowSeq);
        if (b.planOutside) notes << QString::fromUtf8("计划外落格");
        b.note = notes.join(QString::fromUtf8(" / "));

        rows.append(b);
    }

    // 超计划的 SKU 个数（按 波次+SKU 统计，不按条数）
    int overSkuCount = 0;
    for (auto it = skuEpcs.constBegin(); it != skuEpcs.constEnd(); ++it)
    {
        QSet<QString> wavesOfSku;
        for (const SortingRecord& r : recs)
            if (r.sku == it.key()) wavesOfSku.insert(r.orderCode);
        for (const QString& w : wavesOfSku)
        {
            const int p = planQtyByWaveSku.value(w + "\n" + it.key(), 0);
            if (p > 0 && it.value().size() > p) { ++overSkuCount; break; }
        }
    }

    // ── ⑤ 渲染 13 列 ──
    m_tblRecords->setColumnCount(13);
    m_tblRecords->setHorizontalHeaderLabels({
        QString::fromUtf8("序号"),
        QString::fromUtf8("波次号"),
        QString::fromUtf8("EPC编码"),
        QString::fromUtf8("SKU编码"),
        QString::fromUtf8("实际落格号"),
        QString::fromUtf8("WMS格口编码"),
        QString::fromUtf8("计划格口"),
        QString::fromUtf8("格口类型"),
        QString::fromUtf8("计划数量"),
        QString::fromUtf8("容器号"),
        QString::fromUtf8("落格时间"),
        QString::fromUtf8("状态"),
        QString::fromUtf8("备注")
    });
    m_tblRecords->setRowCount(0);
    m_tblRecords->setRowCount(rows.size());

    int outsideCount = 0, driftCount = 0;
    for (int i = 0; i < rows.size(); ++i)
    {
        const BoxRow& b = rows[i];

        auto* c0 = new QTableWidgetItem(QString::number(i + 1));
        c0->setTextAlignment(Qt::AlignCenter);
        m_tblRecords->setItem(i, 0, c0);

        m_tblRecords->setItem(i, 1, new QTableWidgetItem(b.wave));
        m_tblRecords->setItem(i, 2, new QTableWidgetItem(b.epc));
        m_tblRecords->setItem(i, 3, new QTableWidgetItem(b.sku));

        auto* gridItem = new QTableWidgetItem(b.grid);
        gridItem->setTextAlignment(Qt::AlignCenter);
        gridItem->setToolTip(QString::fromUtf8("WMS编码 %1").arg(b.wmsCode));
        if (b.planOutside)
        {
            gridItem->setForeground(QColor("#FF8C00"));   // 橙色：计划外落格
            QFont f = gridItem->font(); f.setBold(true); gridItem->setFont(f);
        }
        m_tblRecords->setItem(i, 4, gridItem);

        auto* wmsItem = new QTableWidgetItem(b.wmsCode);
        wmsItem->setTextAlignment(Qt::AlignCenter);
        m_tblRecords->setItem(i, 5, wmsItem);

        auto* planItem = new QTableWidgetItem(b.planGrid);
        planItem->setTextAlignment(Qt::AlignCenter);
        if (b.planOutside) planItem->setForeground(QColor("#FF8C00"));
        m_tblRecords->setItem(i, 6, planItem);
        m_tblRecords->setItem(i, 7, new QTableWidgetItem(b.gridType));

        auto* qtyItem = new QTableWidgetItem(b.planQty);
        qtyItem->setTextAlignment(Qt::AlignCenter);
        m_tblRecords->setItem(i, 8, qtyItem);

        m_tblRecords->setItem(i, 9,  new QTableWidgetItem(b.box));
        m_tblRecords->setItem(i, 10, new QTableWidgetItem(b.time));

        auto* statusItem = new QTableWidgetItem(QString::fromUtf8("已分拣"));
        statusItem->setTextAlignment(Qt::AlignCenter);
        statusItem->setForeground(b.planOutside ? QColor("#FF8C00") : QColor("#228B22"));
        m_tblRecords->setItem(i, 11, statusItem);

        auto* noteItem = new QTableWidgetItem(b.note);
        noteItem->setTextAlignment(Qt::AlignCenter);
        if (!b.note.isEmpty()) noteItem->setForeground(QColor("#D32F2F"));
        // ★ 超出计划的件：整行标红加粗，现场一眼就能挑出"多出来的那一件"
        if (b.skuOverflowRow || b.drift)
        {
            QFont bold = noteItem->font();
            bold.setBold(true);
            noteItem->setFont(bold);
            m_tblRecords->item(i, 2)->setForeground(QColor("#D32F2F"));   // EPC 列
            m_tblRecords->item(i, 3)->setForeground(QColor("#D32F2F"));   // SKU 列
        }
        m_tblRecords->setItem(i, 12, noteItem);

        if (b.planOutside) ++outsideCount;
        if (b.drift)       ++driftCount;
    }

    m_tblRecords->verticalHeader()->setDefaultSectionSize(32);

    // ── ⑥ 统计标签：「同格口逐箱对照 + 本容器汇总」──
    QStringList seg;

    // 同格口其他容器逐箱对照（按落格先后=容器切换顺序）
    QStringList boxesOfGrid;
    for (const SortingRecord& r : recs)
    {
        const QString bc = r.boxcode.trimmed();
        if (!bc.isEmpty() && !boxesOfGrid.contains(bc)) boxesOfGrid << bc;
    }
    if (!gridsInBox.isEmpty())
    {
        for (const QString& g : gridsInBox)
        {
            QStringList parts;
            for (const QString& bc : boxesOfGrid)
            {
                int cnt = 0;
                for (const SortingRecord& r : recs)
                    if (gridKeyOf(r.gridNum) == g && r.boxcode.trimmed() == bc) ++cnt;
                if (cnt > 0)
                    parts << QString::fromUtf8("%1:%2件").arg(bc).arg(cnt);
            }
            if (!parts.isEmpty())
                seg << QString::fromUtf8("同格口%1逐箱 [%2]").arg(g).arg(parts.join(QString::fromUtf8(" → ")));
        }
    }

    seg << QString::fromUtf8("本容器 [%1]：EPC 去重 %2 件（明细 %3 条）｜SKU %4 个｜波次 %5 个｜格口 %6 个｜日期 %7")
            .arg(box).arg(uniqEpcs.size()).arg(recs.size())
            .arg(uniqSkus.size()).arg(uniqWaves.size()).arg(gridsInBox.size()).arg(rangeText);
    seg << QString::fromUtf8("异常：同EPC多容器 %1 条｜超计划 %2 件（%3 个SKU）｜计划外落格 %4 条")
            .arg(driftCount).arg(overflowRowCount).arg(overSkuCount).arg(outsideCount);

    QString label = seg.join(QString::fromUtf8("　｜　"));
    if (recs.size() >= SORTING_QUERY_MAX_RESULTS)
        label += QString::fromUtf8("（已达单次查询上限 %1 条，可能截断）").arg(SORTING_QUERY_MAX_RESULTS);

    m_lblRecordCount->setText(label);
    m_lblRecordCount->setToolTip(QString::fromUtf8(
        "按容器号查询口径：\n"
        "  ★ 日期区间（必填）作用于落格时间 sort_time：只统计区间内的落格明细\n"
        "  行 = 该容器号下的一条落格明细（每条明细对应 1 个 EPC）\n"
        "  EPC 去重 = 该容器内不重复实物件数（同一 EPC 重复反馈/重投只算 1 件）\n"
        "  计划数量/计划格口 = 该波次该 SKU 的 WMS 计划（来自波次明细 return_wave_item）\n"
        "  备注四类提示（红色行 = 关注项）：\n"
        "    同EPC多容器[x,y] —— 同一件也曾落到别的容器（重扫重投+中途换箱的典型症状）\n"
        "    本容器重复N条 —— 同一 EPC 在本容器有多条明细（重复反馈/重投）\n"
        "    ★超出计划第N件 —— 该 SKU 在本容器已累计超过计划件数，本行就是多出来的那件\n"
        "    计划外落格 —— 实际落格格口不在该 SKU 的计划格口内\n"
        "  最后一栏「同格口逐箱」按容器切换顺序给出各箱件数，可直接看出跨箱漂移\n"
        "双击任意行可查看该 EPC 的全信息（分拣历史/异常/计划明细）"));
    m_lblRecordCount->setStyleSheet(
        (driftCount > 0 || overflowRowCount > 0 || outsideCount > 0 || uniqWaves.size() > 1)
            ? "font-size: 13px; color: #D32F2F; font-weight: bold;"
            : "font-size: 13px; color: #555; font-weight: bold;");

    // ── ⑦ 运行日志留痕 ──
    appendLog(QString::fromUtf8("[查询] 容器 [%1]（日期 %2）：EPC 去重 %3 件（明细 %4 条）、SKU %5 个、波次 %6 个")
        .arg(box).arg(rangeText).arg(uniqEpcs.size()).arg(recs.size()).arg(uniqSkus.size()).arg(uniqWaves.size()));
    if (driftCount > 0 || overflowRowCount > 0 || outsideCount > 0)
    {
        appendLog(QString::fromUtf8("[查询] 容器 [%1] 发现异常：同EPC多容器 %2 条、超计划 %3 件（%4 个SKU）、计划外落格 %5 条")
            .arg(box).arg(driftCount).arg(overflowRowCount).arg(overSkuCount).arg(outsideCount), true);
    }
    if (recs.isEmpty())
        appendLog(QString::fromUtf8("[查询] 容器 [%1] 在日期 %2 内无落格记录（请确认容器号/日期区间，或该容器尚未有物件落入）")
            .arg(box).arg(rangeText), true);
}


// ★ onQueryRecords — 分拣记录查询（按EPC / 按SKU查格口 / 按格口查询 / 按容器号查询）
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

    // ══════════════════════════════════════════════════════════════════════════
    // ★ 2026-09-16 现场需求③：**日期区间（必填）** —— 四个查询模式统一口径
    //   · 起 = 起始日 00:00:00，止 = 结束日 23:59:59（含首含尾），命中列 = sort_time（落格时间）
    //   · 「待分拣」计划行（return_wave_item）没有落格时间，**不受日期筛选影响**（由 DB 层保证）
    //   · 起 > 止 时自动对调并告警（不静默返回空结果）
    // ══════════════════════════════════════════════════════════════════════════
    QDate dateFrom = m_editQueryDateFrom->date();
    QDate dateTo   = m_editQueryDateTo->date();
    if (dateFrom > dateTo)
    {
        appendLog(QString::fromUtf8("[查询] 日期区间无效（起 %1 > 止 %2），已自动对调后查询")
            .arg(dateFrom.toString("yyyy-MM-dd")).arg(dateTo.toString("yyyy-MM-dd")), true);
        qSwap(dateFrom, dateTo);
    }
    const QDateTime qFrom(dateFrom, QTime(0, 0, 0));
    const QDateTime qTo(dateTo, QTime(23, 59, 59));
    const QString   rangeText = QString::fromUtf8("%1 ~ %2")
        .arg(dateFrom.toString("yyyy-MM-dd")).arg(dateTo.toString("yyyy-MM-dd"));

    if (queryMode == 2)
    {
        // ★ 2026-09-09 需求2：按格口查询分拣数量（留空=全格口汇总，输入格口号=该格明细）
        QString grid = m_editQueryBarcode->text().trimmed();

        if (grid.isEmpty())
        {
            // ── 全格口汇总：每格一行（格口号/分拣数量/SKU数/容器号/最近分拣时间）──
            //   ★ 需求③：计数/SKU数/最近容器号/最近时间都只反映所选日期区间内的落格
            QVector<GridSummaryRecord> sums = db->queryGridSummary(qFrom, qTo);
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
            m_lblRecordCount->setText(QString::fromUtf8("全格口汇总（日期 %1）：%2 个格口，共 %3 件")
                .arg(rangeText).arg(sums.size()).arg(totalItems));
            appendLog(QString::fromUtf8("[查询] 按格口汇总（日期 %1）：%2 个格口，共 %3 件")
                .arg(rangeText).arg(sums.size()).arg(totalItems));
        }
        else
        {
            // ── 该格分拣明细 ──
            // ★ 2026-09-10/11 查询兼容：输入 "7"（裸数字）/ "007"（补零）/ "22007"（WMS编码）等写法，
            //   统一在内部归一成同一个内部格口 key 后再查询——UI 表现与结果完全一致（无差异）
            const QString gridKey = gridKeyOf(grid);
            const QString wmsCode = gridToWmsCode(gridKey);   // 内部 key → WMS 编码（"007" → "22007"）

            // ★ 需求③：按格口查询同样只取所选日期区间内的落格明细
            QVector<SortingRecord> recs = db->queryByGrid(gridKey, qFrom, qTo, SORTING_QUERY_MAX_RESULTS);
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
            m_lblRecordCount->setText(QString::fromUtf8("格口 [%1]（WMS编码 %2）分拣数量：%3 件（日期 %4）")
                .arg(gridKey).arg(wmsCode).arg(recs.size()).arg(rangeText));
            // ★ 2026-09-11：日志不体现原始输入写法（"7"/"007"/"22007" 完全一致，UI 无差异）
            appendLog(QString::fromUtf8("[查询] 格口 [%1]（WMS编码 %2）分拣数量：%3 件（日期 %4）")
                .arg(gridKey).arg(wmsCode).arg(recs.size()).arg(rangeText));
            if (recs.isEmpty())
            {
                appendLog(QString::fromUtf8("[查询] 格口 [%1] 在日期 %2 内无分拣记录")
                    .arg(gridKey).arg(rangeText), true);
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

    if (queryMode == 3)
    {
        renderContainerQuery();
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
        //   ① querySkuGridMapping：WMS 下发的计划格口（计划视角）——★ 计划与波次绑定，不受日期筛选
        //   ② queryBySku：sorting_records 落格明细，每条=1 个 EPC，grid_num 即实际落格号（实绩视角）
        //      ★ 2026-09-16 需求③：**只取所选日期区间内的落格明细**（区间外的 EPC 不出现）
        //   一行 = 一个 EPC ↔ 其实际落格号（无落格的计划格口单独出一行"待分拣"）
        QVector<ReturnWaveItemRecord> items = db->querySkuGridMapping(sku);
        QVector<SortingRecord> details = db->queryBySku(sku, qFrom, qTo, SORTING_QUERY_MAX_RESULTS);

        // 波次 → 该 SKU 的计划格口列表（判断"实际落格号是否计划外"）
        QMap<QString, QStringList> planGridsByWave;
        QMap<QString, QString>     planBoxByWave;   // ★ 波次 → WMS 下发的容器号（落格容器为空时兜底）
        for (const ReturnWaveItemRecord& it : items)
        {
            planGridsByWave[it.orderCode] << gridKeyOf(it.gridNum);
            if (!it.obxCode.isEmpty() && !planBoxByWave.contains(it.orderCode))
                planBoxByWave[it.orderCode] = it.obxCode;
        }

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
        int mismatchCount = 0;          // ★ 2026-09-13 实际落格号不在计划格口内的条数（"计划外"）

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
        QSet<QString> uniqEpcs;   // ★ 2026-09-13 去重 EPC（件数口径，区别于"行数"）
        for (int k = 0; k < details.size(); ++k)
        {
            const SortingRecord& d = details[k];
            uniqEpcs.insert(d.barcode);
            if (used[k]) continue;

            SkuRow r;
            r.wave       = d.orderCode;
            r.epc        = d.barcode;
            r.actualGrid = gridKeyOf(d.gridNum);
            // ★ 2026-09-13：计划外/跨波次 EPC 回填真实计划格口（首落格口），避免"计划格口"列空白
            const QString firstGrid = db->getFirstSortedGrid(d.orderCode, d.barcode);
            QStringList planList = planGridsByWave.value(d.orderCode);
            r.planGrid   = firstGrid.isEmpty() ? planList.join("/") : firstGrid;
            r.volu       = d.volu;
            r.box        = d.boxcode.isEmpty() ? planBoxByWave.value(d.orderCode) : d.boxcode;
            r.time       = d.sortTime;
            r.status     = QString::fromUtf8("已分拣");
            if (!r.planGrid.isEmpty())
            {
                r.planGrid += QString::fromUtf8("（计划外）");
                r.mismatch  = true;
            }
            if (r.mismatch) ++mismatchCount;   // ★ 2026-09-13 统计"计划外"条数
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
        // ★ 2026-09-13 字体放大后：不再逐行 resizeRowsToContents（1000 行会明显卡顿），
        //   改用统一行高（32px），视觉效果一致且渲染开销恒定
        m_tblRecords->verticalHeader()->setDefaultSectionSize(32);

        // ── 统计标签（★ 2026-09-13 口径分三段：计划 / 落格 EPC / 去重 EPC / 计划外）──
        //   ★ 2026-09-16 需求③：落格/去重/计划外 只统计所选日期区间；计划明细与日期无关
        QString label = QString::fromUtf8(
            "SKU编码 [%1]：计划 %2 条明细（%3 个格口）｜EPC 落格 %4 条 ｜ 去重 %5 个 EPC（实际落在 %6 个格口）｜ 计划外 %7 条 ｜ 日期 %8")
            .arg(sku).arg(planRowCount).arg(planGridSet.size())
            .arg(epcRowCount).arg(uniqEpcs.size()).arg(actualGridSet.size())
            .arg(mismatchCount).arg(rangeText);
        if (details.size() >= SORTING_QUERY_MAX_RESULTS)
            label += QString::fromUtf8("（已达单次查询上限 %1 条，可能截断）").arg(SORTING_QUERY_MAX_RESULTS);
        m_lblRecordCount->setText(label);
        m_lblRecordCount->setToolTip(QString::fromUtf8(
            "按 SKU 查询口径：\n"
            "  ★ 日期区间（必填）作用于落格时间 sort_time：EPC 落格/去重/计划外只统计区间内的明细\n"
            "  计划 = WMS 下发的该 SKU 格口分配明细（一条=一个格口分配）——计划与波次绑定，不受日期筛选\n"
            "  EPC 落格 = 该 SKU 下每个 EPC 的实际落格记录（一行 = 一个 EPC）\n"
            "  去重 EPC = 不重复实物件数；计划外 = 实际落格号不在计划格口内的条目\n"
            "  「待分拣」行 = 有计划但（该日期区间内）无落格 EPC，照常显示\n"
            "双击任意行可查看该 EPC 的全信息（分拣历史/异常历史/计划明细）"));
        // 同品多格口：高亮显示（沿用原口径）
        m_lblRecordCount->setStyleSheet(planGridSet.size() > 1
            ? "font-size: 14px; color: #FF8C00; font-weight: bold;"
            : "font-size: 14px; color: #555; font-weight: bold;");

        // 更新数据库统计
        SortingStatistics stats = db->statistics();
        m_lblDbStats->setText(QString::fromUtf8("数据库: 总计 %1 条 | 今日 %2 条 | %3 波次 | %4 格口")
            .arg(stats.totalRecords)
            .arg(stats.todayRecords)
            .arg(stats.totalWaves)
            .arg(stats.totalGrids));

        appendLog(QString::fromUtf8("[查询] SKU编码 [%1]（日期 %2）：计划 %3 条明细（%4 个格口），EPC落格 %5 条，去重 %6 个 EPC（实际落在 %7 个格口）")
            .arg(sku).arg(rangeText).arg(planRowCount).arg(planGridSet.size())
            .arg(epcRowCount).arg(uniqEpcs.size()).arg(actualGridSet.size()));
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

    QVector<SortingRecord> records;

    if (!barcode.isEmpty())
    {
        // 按EPC编码查询（已分拣 + 待分拣，用 NOT EXISTS 去重）
        // ★ 需求③：已分拣部分只取日期区间内；「待分拣」计划行不受日期影响（DB 层保证）
        records = db->queryByBarcode(barcode, qFrom, qTo, SORTING_QUERY_MAX_RESULTS);
    }
    else
    {
        // 留空查全部：已分拣 + 待分拣（queryAllWithPending 自动用 NOT EXISTS 去重）
        records = db->queryAllWithPending(qFrom, qTo, SORTING_QUERY_MAX_RESULTS);
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

    // 更新统计标签（★ 需求③：文案统一带日期区间，便于现场核对"查的是哪一段"）
    if (!barcode.isEmpty())
    {
        m_lblRecordCount->setText(QString::fromUtf8("共 %1 条记录（EPC编码: %2 ｜ 日期 %3）")
            .arg(records.size()).arg(barcode).arg(rangeText));
    }
    else
    {
        m_lblRecordCount->setText(QString::fromUtf8("共 %1 条记录（日期 %2）")
            .arg(records.size()).arg(rangeText));
    }
    m_lblRecordCount->setToolTip(QString::fromUtf8(
        "按 EPC 查询口径：\n"
        "  ★ 日期区间（必填）作用于落格时间 sort_time：已分拣记录只显示区间内的\n"
        "  ★「待分拣」计划行没有落格时间，**不受日期筛选影响**，照常显示（状态列标橙）\n"
        "  状态列对异常件显示异常原因（红色）\n"
        "双击任意行可查看该 EPC 的全信息（分拣历史/异常历史/计划明细）"));

    // 更新数据库统计
    SortingStatistics stats = db->statistics();
    m_lblDbStats->setText(QString("数据库: 总计 %1 条 | 今日 %2 条 | %3 波次 | %4 格口")
        .arg(stats.totalRecords)
        .arg(stats.todayRecords)
        .arg(stats.totalWaves)
        .arg(stats.totalGrids));

    appendLog(QString::fromUtf8("[查询] 返回 %1 条记录（日期 %2）").arg(records.size()).arg(rangeText));
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
        // ★ 2026-09-15 开工前「计划格口 vs 容器绑定」预检：
        //   每个格口（正常分拣/发货）各有自己的计划件数，按数量分配的前提是这些格口都有容器可落。
        //   这里把"计划里有件但未绑定容器/已禁用"的格口一次性列出来（只告警不阻塞）。
        if (m_pServer)
            m_pServer->precheckPlanGridBindings(wm->orderCode());
        // 按钮由定时器自动刷新为灰色禁用状态（状态已变为 SORTING）
    }
    else
    {
        appendLog("[分拣] 开始分拣失败，请检查波次状态", true);
    }
}

// ============================================================================
// ★ 2026-09-13 需求：一键满箱回传
//   点击 → 对**当前所有已绑定容器**逐个执行 H7 满箱回传，同批自动统计。
//
//   设计要点（确保"不影响其他功能"）：
//     · 复用 manualFullbox → sendFullboxForGrid：不动波次状态机、不禁用格口、不改容器绑定；
//     · 只对"有分拣记录"的格口发送（无记录的格口跳过并计数提示，不发空报文）；
//     · 失败不阻塞：报文已入 Outbox，失败/超时由既有重传机制处理（可面板重传）；
//     · 已满箱回传过的格口（记录已清空）再点不会重复发送——sendFullboxForGrid 的入参为空直接返回。
// ============================================================================
void MainWindow::onOneKeyFullbox()
{
    if (!m_pServer || !m_pServer->waveManager())
    {
        appendLog("[一键满箱] 服务未就绪，无法执行", true);
        return;
    }
    if (m_btnOneKeyFullbox) m_btnOneKeyFullbox->setEnabled(false);   // 防连点（动作末尾恢复）

    WaveManager* wm = m_pServer->waveManager();
    const QString orderCode = wm->orderCode();
    if (orderCode.isEmpty())
    {
        appendLog("[一键满箱] 当前无运行波次，无法执行一键满箱回传", true);
        if (m_btnOneKeyFullbox) m_btnOneKeyFullbox->setEnabled(true);
        return;
    }

    // 当前全部绑定（格口号 → 容器号）
    const QMap<QString, QString> binds = m_pServer->getContainerBindings();
    if (binds.isEmpty())
    {
        appendLog("[一键满箱] 当前无已绑定容器，无可回传内容", true);
        if (m_btnOneKeyFullbox) m_btnOneKeyFullbox->setEnabled(true);
        return;
    }

    // 二次确认：涉及批量回传（每条一报、各自入 Outbox），避免误触
    const int total = binds.size();
    if (QMessageBox::question(this, QString::fromUtf8("一键满箱回传"),
            QString::fromUtf8("将对当前 %1 个已绑定容器逐个执行满箱回传(H7)：\n"
                              "· 只有存在分拣记录的格口会发送报文，无记录的格口自动跳过\n"
                              "· 不影响分拣流程、波次状态与容器绑定\n\n确认执行？").arg(total),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    {
        appendLog("[一键满箱] 已取消");
        if (m_btnOneKeyFullbox) m_btnOneKeyFullbox->setEnabled(true);
        return;
    }

    appendLog(QString("[一键满箱] 开始执行：已绑定容器 %1 个，波次 %2").arg(total).arg(orderCode));

    int sent = 0, empty = 0, failed = 0;
    QStringList sentGrids, emptyGrids, failedGrids;
    for (auto it = binds.constBegin(); it != binds.constEnd(); ++it)
    {
        const QString grid = it.key();
        const QString box  = it.value();

        // manualFullbox（★ 需求④）：成功返回本次生成的 H7 msgId（已入 Outbox）；失败返回空串 + 原因
        //   原因区分："无待上传的分拣记录" = 正常跳过（该格已满箱或未落格）；
        //             其余（无容器绑定/缺SKU/Outbox 写入失败/异常口…）= 真失败，计入失败数
        QString reason;
        const QString msgId = m_pServer->manualFullbox(grid, &reason);
        if (!msgId.isEmpty())
        {
            ++sent;
            m_oneKeyMsgIds.insert(msgId);   // ★ 需求④：登记本次一键生成的报文，供失败回执归因
            sentGrids << QString::fromUtf8("%1(%2)").arg(grid, box);
        }
        else if (reason == QString::fromUtf8("无待上传的分拣记录"))
        {
            ++empty;                        // 正常跳过（不算失败，沿用既有日志口径）
            emptyGrids << grid;
        }
        else
        {
            ++failed;                       // ★ 需求④：真失败（未能生成报文）
            ++m_oneKeyFails;
            failedGrids << QString::fromUtf8("%1(%2)").arg(grid).arg(
                reason.isEmpty() ? QString::fromUtf8("未知原因") : reason);
        }
    }
    m_oneKeyBatches += sent;                // ★ 需求④：本次一键成功生成报文数（会话累计）

    QString msg = QString::fromUtf8(
        "[一键满箱] 执行完成：已发送 %1 个格口（%2）｜跳过无记录 %3 个（%4）｜失败 %5 个（%6）")
        .arg(sent)
        .arg(sentGrids.isEmpty() ? QString::fromUtf8("无") : sentGrids.join(","))
        .arg(empty)
        .arg(emptyGrids.isEmpty() ? QString::fromUtf8("无") : emptyGrids.join(","))
        .arg(failed)
        .arg(failedGrids.isEmpty() ? QString::fromUtf8("无") : failedGrids.join(","));
    appendLog(msg, sent == 0 && failed > 0);
    appendLog(QString::fromUtf8(
        "[一键满箱] 提示：报文已入 Outbox 异步发送；失败/超时的报文可在「重传满箱切换(H7)」下拉中选择重传"));

    // ★ 需求④：刷新按钮旁的计数文字（本波次总数 + 本次一键成功/失败），并追加同源计数日志
    refreshFullboxCountLabel();
    appendLog(QString::fromUtf8(
        "[一键满箱] 计数：%1")
        .arg(m_lblFullboxCount ? m_lblFullboxCount->text() : QString()));

    if (m_btnOneKeyFullbox) m_btnOneKeyFullbox->setEnabled(true);
}

