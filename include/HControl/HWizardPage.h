#ifndef _HWIZARDPAGE_H_
#define _HWIZARDPAGE_H_

#include "hcontrol_global.h"
#include <QWidget>



class HCONTROL_EXPORT HWizardPage : public QWidget
{
	Q_OBJECT
signals:

	// 移动到前置界面
	void moveByOrient(int pageType, HWIZARD_ORIENT orient);
protected:
	virtual void raiseMovePrevious();

	virtual void raiseMoveNext();

public:
	HWizardPage(QWidget* parent = Q_NULLPTR);

	void setTitle(QString title);

	QString getTitle();

	void setPageType(int pageType);

	int getPageType();

	void setPos(int pos);

	int getPos();

    void setIsShow(bool bIsShow);

    bool getIsShow();

	// 导航进入
	virtual void navigateIn();

	// 导航退出
	virtual void navigateOut();


private:
	int      m_nPos;
	int      m_nPageType;
	QString  m_strTitle;
    bool     m_bIsShow;
};

#endif