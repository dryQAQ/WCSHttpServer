#ifndef _HMESSAGEBOX_H_
#define _HMESSAGEBOX_H_

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
class HCONTROL_EXPORT HMessageBox : public QDialog
{
	Q_OBJECT
signals:
	void btnOkClicked();
	void btnCancelClicked();

public:
	HMessageBox(HMessageBoxType type, QString content);

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
	QHBoxLayout*      m_pTitleLayout;
	QPushButton*      m_pBtnClose;
	QLabel*           m_pLabContent;
	QHBoxLayout*      m_pBtnLayout;
	QPushButton*      m_pBtnOk;
	QPushButton*      m_pBtnCancel;

};

class HCONTROL_EXPORT HLoadingBox : public QDialog
{
	Q_OBJECT


public:
	HLoadingBox(QWidget* parent = Q_NULLPTR);
	~HLoadingBox();

	void finish();

	static void ShowLoading();

	static void HideLoading();

protected:
	bool nativeEvent(const QByteArray &eventType, void *message, long *result);

private:
	static HLoadingBox* _box;

private:
	QMovie*    movie;
	QLabel*    label;
};


#endif