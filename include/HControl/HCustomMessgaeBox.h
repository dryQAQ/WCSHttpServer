#ifndef _HCUSTOMMESSAGEBOX_H_
#define _HCUSTOMMESSAGEBOX_H_

#include "hcontrol_global.h"
#include <QWidget>
#include <QDialog>
#include <QResizeEvent>


class QVBoxLayout;
class QPushButton;
class QHBoxLayout;
class QLabel;
class QFrame;
class QMovie;
class HCONTROL_EXPORT HCustomMessageBox : public QDialog
{
    Q_OBJECT
        signals :
    void btnOkClicked();
    void btnCancelClicked();

public:
    HCustomMessageBox(HMessageBoxType type, QString content);

    HMessageBoxResult static warnning(QString content);

    HMessageBoxResult static show(QString content);

protected:
    void resizeEvent(QResizeEvent* event);

    private slots:
    void onBtnOkClicked();
    void onBtnCancelClicked();
private:
    QFrame*           m_pMainFrame;
    QVBoxLayout*      m_pMainLayout;
    QWidget*          m_pTitleWidget;
    QHBoxLayout*      m_pTitleLayout;
    QLabel*           m_pLabTitle;
    QPushButton*      m_pBtnClose;
    QLabel*           m_pLabContentImage;
    QLabel*           m_pLabContent;
    QHBoxLayout*      m_pBtnLayout;
    QPushButton*      m_pBtnOk;
    QPushButton*      m_pBtnCancel;

};


#endif