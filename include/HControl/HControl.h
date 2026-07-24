#ifndef _HCONTROL_H_
#define _HCONTROL_H_

#include "hcontrol_global.h"
#include <QWidget>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>

class QTextEdit;
class QLabel;
class QString;
class QHBoxLayout;
class QResizeEvent;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QSlider;
class QVBoxLayout;

class HCONTROL_EXPORT HEditBase : public QWidget
{
	Q_OBJECT

signals:

	void dataChanged(QString name, QString value);

public:
	HEditBase(QWidget* parent = Q_NULLPTR);
	virtual ~HEditBase();

	void setTag(QString tag);

	QString tag();

	virtual void setStringValue(QString val);

protected:
	QString m_strTag;
};

class HCONTROL_EXPORT HTextEdit : public HEditBase
{
	Q_OBJECT

public:
	HTextEdit(QWidget* parent = Q_NULLPTR);
	~HTextEdit();

	void setTitle(QString title);

	void setValue(QString value);

	QString getValue();

	void setPasswordMode(bool isPasswordMode);

	void setStringValue(QString val) override;

protected:
	void resizeEvent(QResizeEvent* event) override;

private slots:

    void on_textChanged(const QString &text);

	void on_editingFinished();

private:
	QLabel*            m_pLabTitle;
	QLineEdit*         m_pTxtContent;

	QString            m_oldValue;
};

class HCONTROL_EXPORT HLineLabel : public HEditBase
{
	Q_OBJECT

public:
	HLineLabel(QWidget* parent = Q_NULLPTR);
	~HLineLabel();

	void setTitle(QString title);

	void setValue(QString value);


protected:
	void resizeEvent(QResizeEvent* event) override;

private:
	QLabel*            m_pLabTitle;
	QLabel*            m_pLabContent;
};

class HCONTROL_EXPORT Switch : public QLabel
{
	Q_OBJECT

signals:

	void SwitchStateChanged();

public:
	Switch(QWidget* parent = Q_NULLPTR);
	~Switch();

	// 打开
	void turnOn();

	// 关闭
	void turnOff();

	bool isOn();

protected:

	void mousePressEvent(QMouseEvent *ev) override;

private:
	QPixmap onPixmap;
	QPixmap offPixmap;

	bool m_bOn;
};

class HCONTROL_EXPORT HSwitch : public HEditBase
{
	Q_OBJECT
signals :

	void SwitchStateChanged();

public:
	HSwitch(QWidget* parent = Q_NULLPTR);
	~HSwitch();

	void setTitle(QString title);

	void setChecked(bool isChecked);

	void setPixelFontSize(int size);

	bool isChecked();

	void setStringValue(QString val) override;

protected:

	void resizeEvent(QResizeEvent* event);

private slots:

    void on_switch_stateChanged();

private:
	QLabel*       m_pLabTitle;
	Switch*       m_pSwitch;

	bool          m_oldValue;
};

class HCONTROL_EXPORT HComboBox : public HEditBase
{
	Q_OBJECT

signals :

	void currentIndexChanged(int i);

public:
	HComboBox(QWidget* parent = Q_NULLPTR);
	~HComboBox();

	void setTitle(QString title);

	void addItem(QString itemName);

	void addItems(QStringList itemNames);

	void setCurrentItemIndex(int index);

	int currentItemIndex();

	void setStringValue(QString val) override;

protected:
	void resizeEvent(QResizeEvent* event) override;

private slots:

    void on_combobox_currentIndexChanged(int index);

private:
	QLabel*            m_pLabTitle;
	QComboBox*         m_pCmbValue;

	int                m_oldValue;
};

class HCONTROL_EXPORT QSpinBoxEx : public QSpinBox
{
	Q_OBJECT
signals:
	void mouseClicked();

	void hideTriggered();
public:
	QSpinBoxEx(QWidget* parent = Q_NULLPTR);
	~QSpinBoxEx();

protected:

	//bool event(QEvent* event) override;
	////void leaveEvent(QEvent *event) override;

	//void mousePressEvent(QMouseEvent* event) override;

	bool eventFilter(QObject *obj, QEvent *event) override;

public:
};

class HCONTROL_EXPORT QSliderEx : public QWidget
{
	Q_OBJECT
signals:

	void hideTriggered();

	void valueChanged(int val);

public:
	QSliderEx(QWidget* parent = Q_NULLPTR);
	~QSliderEx();

	void setOrientation(Qt::Orientation orientation);

	void setRange(int min, int max);

	void setSingleStep(int inc);

	int value();

	void setValue(int val);

protected:

	void leaveEvent(QEvent *event) override;

	void paintEvent(QPaintEvent* event) override;


	virtual void hideEvent(QHideEvent *event) override;


private:
	QVBoxLayout*    m_pMainLayout;
	QSlider*        m_pSlider;


};

class HCONTROL_EXPORT QSpinBoxEx2 : public QWidget
{
	Q_OBJECT
signals:

	void valueChanged(int i);

public:
	QSpinBoxEx2(QWidget* parent = Q_NULLPTR);
	~QSpinBoxEx2();

	void setValue(int value);

	int value();

	void setRange(int min, int max);

	void setSingleStep(int step);

protected:
	void resizeEvent(QResizeEvent* event);

private slots:
    void onSpinBox_mouseClicked();


	void onSlider_hideTriggered();

	void onSlider_valueChanged(int i);

	void onSpinBox_valueChanged(int i);

private:
	QSpinBoxEx*     m_pSpinBox;
	QSliderEx*      m_pSlider;
};


class HCONTROL_EXPORT HSpinBox : public HEditBase
{
	Q_OBJECT

signals:

	void valueChanged(int i);

public:
	HSpinBox(QWidget* parent = Q_NULLPTR);
	~HSpinBox();

	void setTitle(QString title);

	void setRange(int minVal, int maxVal);

	void setIncrement(int incVal);

	void setValue(int val);

	int getValue();

	void setStringValue(QString val) override;

protected:
	void resizeEvent(QResizeEvent* event) override;

private slots:

    void on_spinbox_valueChanged(int i);

private:
	QLabel*            m_pLabTitle;
	QSpinBoxEx2*        m_pSpinValue;

	int                m_oldValue;
};

class HCONTROL_EXPORT QDoubleSpinBoxEx : public QDoubleSpinBox
{
	Q_OBJECT
signals :
	void mouseClicked();

	void hideTriggered();

public:
	QDoubleSpinBoxEx(QWidget* parent = Q_NULLPTR);
	~QDoubleSpinBoxEx();

	//void setDecemial(int value);

private slots:

	void onLineEditClicked();

protected:

	bool eventFilter(QObject *obj, QEvent *event);

};

class HCONTROL_EXPORT QDoubleSpinBoxEx2 : public QWidget
{
	Q_OBJECT
signals:

	void valueChanged(double i);
public:
	QDoubleSpinBoxEx2(QWidget* parent = Q_NULLPTR);
	~QDoubleSpinBoxEx2();

	void setValue(double value);

	double value();

	void setRange(double min, double max);

	void setSingleStep(double step);

	void setDeceimal(int value);

protected:
	void resizeEvent(QResizeEvent* event) override;



private slots:
	void onSpinBox_mouseClicked();

	void onSlider_hideTriggered();

	void onSlider_valueChanged(int i);

	void onSpinBox_valueChanged(double i);


private:
	QDoubleSpinBoxEx*     m_pSpinBox;
	QSliderEx*        m_pSlider;
};


class HCONTROL_EXPORT HDoubleSpinBox : public HEditBase
{
	Q_OBJECT

signals:

	void valueChanged(double i);

public:
	HDoubleSpinBox(QWidget* parent = Q_NULLPTR);
	~HDoubleSpinBox();

	void setTitle(QString title);

	void setRange(double minVal, double maxVal);

	void setIncrement(double incVal);

	void setValue(double val);

	double getValue();
	void setDeceimal(int value);

	void setStringValue(QString val) override;

protected:
	void resizeEvent(QResizeEvent* event) override;

private slots:

    void on_spinbox_valueChanged(double d);

private:
	QLabel*            m_pLabTitle;
	QDoubleSpinBoxEx2*    m_pSpinValue;

	double              m_oldValue;
};

class HCONTROL_EXPORT HSpinBoxBetween : public HEditBase
{
	Q_OBJECT

		signals :

	void valueChanged(int i);

public:
	HSpinBoxBetween(QWidget* parent = Q_NULLPTR);
	~HSpinBoxBetween();

	void setTitle(QString title);

	void setRange(int minVal, int maxVal);

	void setIncrement(int incVal);

	void setValue(int minVal, int maxVal);

	int getMinValue();

	int getMaxValue();

	void setStringValue(QString val) override;

protected:
	//void resizeEvent(QResizeEvent* event) override;

private slots:

	//void on_spinbox_valueChanged(int i);

private:
	QLabel*            m_pLabTitle;
	QSpinBoxEx2*       m_pSpinLowValue;
	QSpinBoxEx2*       m_pSpinUpValue;
	int                m_minRange;
	int                m_maxRange;
};

#endif