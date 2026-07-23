#pragma once
#include <QString>
#include <mutex>
#include <QObject>
#include <thread>
#include <QTextEdit>

class LogCenter :public QObject
{
    Q_OBJECT

signals:
    void sig_log_run(bool b, QString msg);
    void sig_plc(bool b, QString msg);
    void sig_upload_run(bool b, QString msg);
    void sig_test_Log(QString msg);

public:
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

    void set_test_window(QTextEdit* text)
    {
        m_test_Window = text;
    }

    void setRunTextBox(QTextEdit* text)
    {
        m_runText = text;
    }
    void setPlcTextBox(QTextEdit* text)
    {
        m_plcText = text;
    }
    void setWCSUploadTextBox(QTextEdit* text)
    {
        m_UploadText = text;
    }
    void wcs_run_log_warn(bool normalOrRed, QString msg, bool disGUI = true);

    void plc_run_log_warn(bool b, QString msg);

    void uploadImage_run_log(bool normalOrRed, QString msg, bool disGUI = true);

    void test_log(QString msge);

private:
    QTextEdit* m_runText{ nullptr };
    QTextEdit* m_plcText{ nullptr };
    QTextEdit* m_UploadText{ nullptr };
    QTextEdit* m_test_Window{ nullptr };

};
