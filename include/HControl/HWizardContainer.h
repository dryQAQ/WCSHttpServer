#ifndef _HWIZARDCONTAINER_H_
#define _HWIZARDCONTAINER_H_

#include "hcontrol_global.h"
#include <QWidget>

#define HWIZARDCONTAINER_HEADER_HEIGHT     80
#define HWIZARDCONTAINER_HEADER_WIDTH      200
#define HWIZARDCONTAINER_HEADER_HORIZON_MARGIN 5
#define HWIZARDCONTAINER_HEADER_VERTICAL_MARGIN 15
#define HWIZARDCONTAINER_HEADER_STEP_WIDTH  50


class HWizardPage;
class QRect;
class QPropertyAnimation;
class QParallelAnimationGroup;
class HWizardContainerHeader;
class HWizardContainerHeaderGroup;

// 定制的WizardContainer，因为会渲染3D数据，
// 所以每次切换均重新实例化Page
// 2019.9.23 改为所有Page界面在初始化中均实例化
class HCONTROL_EXPORT HWizardContainer : public QWidget
{
	Q_OBJECT

signals:

	void moveByOrientRequest(int pageType, HWIZARD_ORIENT orient);

	void animationFinished();

public:
	HWizardContainer(HWIZARDCONTAINER_MODE mode,QWidget* parent = Q_NULLPTR);

	void movePrevious(HWizardPage* page);

	void moveNext(HWizardPage* page);

	void InitPage(HWizardPage* page);

	HWizardPage* getOldPage();
	void releaseOldPage();

	void addHeader(QString title, int index);

	void setSelectedHeader(int index);

   
	/* 新的接口，将Page与Title绑定*/
	void movePrevious();

	void moveNext(int pageType);

	void setWizardPageList(QList<HWizardPage*> wizardPageList);

    void updateWizardPageList();

    void setPreviewType(HWIZARDCONTAINER_MODE mode);

    void updateMoveIndex(int pageType);
   
    void updateMovePrevious();

    void updateMoveNext();

protected:

	void resizeEvent(QResizeEvent* event);

	void paintEvent(QPaintEvent* event);

private slots:

    void onAnimationFinished();

    void onHWizardContainerHeaderDoubleClicked(int index);
private:
	void switchPage(HWizardPage* page);

	void displayAnimation(HWIZARD_ORIENT orient);

private:
	QRect           m_recCentralWidget;
	HWizardPage*    m_pCurrentPage;
	HWizardPage*    m_pOldPage;

	QPropertyAnimation* m_pCurrentPageAnimation;
	QPropertyAnimation* m_pOldPageAnimation;
	QParallelAnimationGroup* m_pAnimationGroup;

	QRect                        m_recHeader;
	HWizardContainerHeaderGroup* m_pHeaderGroup;


	QList<HWizardPage*>          m_pWizardPageList;
	QList<HWizardPage*>          m_pShowWizardPageList;
 
    HWIZARDCONTAINER_MODE        m_HWizardContainerMode;

};

class QLabel;
class HWizardContainerHeader :public QWidget
{
	Q_OBJECT
        signals:
    void headerIsDoubleClicked(int index);
public:
	HWizardContainerHeader(QWidget* parent = Q_NULLPTR);

	void setSelected(bool isSelected);

	bool getSelected();

	void setIndex(int index);

	int getIndex();

	void setIsLast(bool isLast);

	bool getIsLast();

	void setHeader(QString header);

protected:
	void paintEvent(QPaintEvent* event);
    void mouseDoubleClickEvent(QMouseEvent *event);
private:
	bool        m_bSelected;
	bool        m_bIsLast;
	int         m_nIndex;

	QLabel*     m_pLabHeader;

};

class HWizardContainerHeaderGroup : public QWidget
{
	Q_OBJECT

public:
	HWizardContainerHeaderGroup(QWidget* parent = Q_NULLPTR);

	void addHeader(QString title, int index);

	void setSelectedHeader(int index);

    int getSelectHeader();

    void removeAll();

	void removeLast();

    QList<HWizardContainerHeader*> getHeaderList();
protected:

	void resizeEvent(QResizeEvent* event);

private:
	void adjustGroup();

private:
	QList<HWizardContainerHeader*> m_lstHeader;
    int m_nCurrentSelectHeader;
};

#endif