#pragma once
// ============================================================================
// log_center.h — GUI日志中心（单例）
//
// 职责：
//   ① 管理4个QTextEdit日志窗口（运行/PLC/上传/测试）
//   ② 提供线程安全的日志写入接口（通过 Qt Signal/Slot 跨线程安全更新UI）
//   ③ 自动截断过长日志（LOG_MAX_TEXT_SIZE=100000 字符）
//   ④ 同时写入 hlog 文件（保留持久化）
//
// 使用方式：
//   LogCenter::Instance()->setRunTextBox(m_ui->txtLog);   // 关联UI控件
//   LogCenter::Instance()->wcs_run_log_warn(true, "msg");  // 写运行日志
//   LogCenter::Instance()->plc_run_log_warn(true, "msg");  // 写PLC日志
//
// 线程安全：使用 QMetaObject::invokeMethod + Qt::QueuedConnection 确保
//           非GUI线程调用时信号槽在GUI线程中执行
// ============================================================================

#include <QString>
#include <mutex>
#include <QObject>
#include <thread>
#include <QTextEdit>

class LogCenter :public QObject
{
    Q_OBJECT

signals:
    // b: 是否正常信息（true=正常绿色, false=警告红色）
    void sig_log_run(bool b, QString msg);         // → Onsig_log_warn → 运行日志
    void sig_plc(bool b, QString msg);             // → Onsig_plc → PLC日志
    void sig_upload_run(bool b, QString msg);      // → Onsig_upload_run → 上传日志
    void sig_test_Log(QString msg);                // → Onsig_test_Log → 测试日志（无颜色区分）

public:
    // 单例（Meyers Singleton, 线程安全）
    static LogCenter* Instance()
    {
        static LogCenter pCenter;
        return &pCenter;
    }

public slots:
    void Onsig_log_warn(bool b, QString msg);
    void Onsig_test_Log(QString msg);
    void Onsig_plc(bool b, QString msg);
    void Onsig_upload_run(bool b, QString msg);

public:
    LogCenter(QObject* parent = nullptr);
    ~LogCenter();

    // ──── UI控件注册 ────
    void setRunTextBox(QTextEdit* text)       { m_runText = text; }     // 运行日志
    void setPlcTextBox(QTextEdit* text)       { m_plcText = text; }     // PLC通信日志
    void setWCSUploadTextBox(QTextEdit* text) { m_UploadText = text; }  // 上传回传日志
    void set_test_window(QTextEdit* text)     { m_test_Window = text; } // 测试日志

    // ──── 日志写入（线程安全，可在任意线程调用）────
    // normalOrRed: true=正常信息(绿色), false=警告/错误(红色)
    // disGUI:   true=仅写hlog文件不更新GUI（默认）, false=同时更新hlog和GUI
    //   注意参数名 disGUI 含义是"disable GUI"——默认不更新GUI，避免HTTPServer性能日志刷屏
    void wcs_run_log_warn(bool normalOrRed, QString msg, bool disGUI = true);
    void plc_run_log_warn(bool normalOrRed, QString msg);
    void uploadImage_run_log(bool normalOrRed, QString msg, bool disGUI = true);
    void test_log(QString msg);

private:
    // ──── 四个QTextEdit指针，分别对应4个日志窗口 ────
    QTextEdit* m_runText{ nullptr };       // 运行日志窗口
    QTextEdit* m_plcText{ nullptr };       // PLC通信日志窗口
    QTextEdit* m_UploadText{ nullptr };    // 上传/回传日志窗口
    QTextEdit* m_test_Window{ nullptr };   // 测试日志窗口
};
