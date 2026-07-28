#include "log_center.h"
#include <QTextCharFormat>
#include "hlog1.h"
#include <QTimer>
#include <QDateTime>
#include "define.h"


void LogCenter::Onsig_upload_run(bool b, QString msg)
{
    try
    {
        if (nullptr == m_UploadText)
            return;
        QTextCharFormat tcf;
        if (b)
        {
            tcf.setForeground(Qt::white);
        }
        else
        {
            tcf.setForeground(Qt::red);
        }

        m_UploadText->moveCursor(QTextCursor::End);

        if (m_UploadText->toPlainText().size() > LOG_MAX_TEXT_SIZE)
        {
            m_UploadText->selectAll();
            m_UploadText->clear();
        }
        QString current_date_time = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss:zzz  ");
        QString ssss = current_date_time + msg + "\n";

        m_UploadText->textCursor().insertText(ssss, tcf);
        LOG_INFO("%s", msg.toLocal8Bit().data());
    }
    catch (...)
    {

    }
}
void LogCenter::Onsig_test_Log(QString msg)
{
    try
    {
        if (nullptr == m_test_Window)
            return;
        QTextCharFormat tcf;
        tcf.setForeground(Qt::red);

        m_test_Window->moveCursor(QTextCursor::End);

        if (m_test_Window->toPlainText().size() > LOG_MAX_TEXT_SIZE)
        {
            m_test_Window->selectAll();
            m_test_Window->clear();
        }
        QString current_date_time = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss:zzz  ");
        QString ssss = current_date_time + msg + "\n";

        m_test_Window->textCursor().insertText(ssss, tcf);
        LOG_INFO("%s", msg.toLocal8Bit().data());
    }
    catch (...)
    {

    }
}

void LogCenter::Onsig_log_warn(bool b, QString msg)
{
    try
    {
        if (nullptr == m_runText)
            return;
        QTextCharFormat tcf;
        if (b)
        {
            tcf.setForeground(Qt::white);
        }
        else
        {
            tcf.setForeground(Qt::red);
        }

        m_runText->moveCursor(QTextCursor::End);

        if (m_runText->toPlainText().size() > LOG_MAX_TEXT_SIZE)
        {
            m_runText->selectAll();
            m_runText->clear();
        }
        QString current_date_time = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss:zzz  ");
        QString ssss = current_date_time + msg + "\n";

        m_runText->textCursor().insertText(ssss, tcf);
        LOG_INFO("%s", msg.toLocal8Bit().data());
    }
    catch (...)
    {

    }
}
void LogCenter::Onsig_plc(bool b, QString msg)
{
    try
    {
        if (nullptr == m_plcText)
            return;
        QTextCharFormat tcf;
        if (b)
        {
            tcf.setForeground(Qt::white);
        }
        else
        {
            tcf.setForeground(Qt::red);
        }

        m_plcText->moveCursor(QTextCursor::End);

        if (m_plcText->toPlainText().size() > LOG_MAX_TEXT_SIZE)
        {
            m_plcText->selectAll();
            m_plcText->clear();
        }
        QString current_date_time = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss:zzz  ");

        QString ssss = current_date_time + msg + "\n";
        m_plcText->textCursor().insertText(ssss, tcf);
        LOG_INFO("%s", msg.toLocal8Bit().data());
    }
    catch (...)
    {

    }
}

LogCenter::LogCenter(QObject* parent) : QObject(parent)
{
    bool b1 = connect(this, &LogCenter::sig_log_run, this, &LogCenter::Onsig_log_warn);
    bool b2 = connect(this, &LogCenter::sig_plc, this, &LogCenter::Onsig_plc);
    //sig_test_Log
    bool b2111 = connect(this, &LogCenter::sig_test_Log, this, &LogCenter::Onsig_test_Log);

}

LogCenter::~LogCenter()
{

}

void LogCenter::wcs_run_log_warn(bool b, QString msg, bool disGUI)
{
    if (disGUI)
        emit sig_log_run(b, msg);
    else
        LOG_INFO("%s", msg.toLocal8Bit().data());
}

void LogCenter::test_log(QString msge)
{
    emit sig_test_Log( msge);
}

void LogCenter::uploadImage_run_log(bool normalOrRed, QString msg, bool disGUI)
{
    if (disGUI)
        emit sig_upload_run(normalOrRed, msg);
    else
        LOG_INFO("%s", msg.toLocal8Bit().data());
}

void LogCenter::plc_run_log_warn(bool b, QString msg)
{
    emit sig_plc(b, msg);
}
