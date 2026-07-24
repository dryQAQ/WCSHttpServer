#ifndef _HCONTROL_GLOBAL_H_
#define _HCONTROL_GLOBAL_H_

#include <QtCore/qglobal.h>

#ifndef BUILD_STATIC
# if defined(HCONTROL_LIB)
#  define HCONTROL_EXPORT Q_DECL_EXPORT
# else
#  define HCONTROL_EXPORT __declspec(dllimport)
# endif
#else
# define HCONTROL_EXPORT
#endif

typedef enum _HTOOLBUTTON_STATUS_
{
	HTOOLBUTTON_STATUS_NORMAL,
	HTOOLBUTTON_STATUS_HOVER,
	HTOOLBUTTON_STATUS_PRESSED,
	HTOOLBUTTON_STATUS_DISABLED
}HTOOLBUTTON_STATUS;

typedef enum _HWIZARDCONTAINER_MODE_
{
    HWIZARDCONTAINER_ADD,
    HWIZARDCONTAINER_UPDATE
}HWIZARDCONTAINER_MODE;

typedef enum _HWIZARD_ORIENT_
{
	HWIZARD_ORIENT_PREVIOUS,
	HWIZARD_ORIENT_NEXT,
	HWIZARD_ORIENT_INIT
}HWIZARD_ORIENT;

typedef enum HMessageBoxResult_H_
{
	HMessageBoxResult_OK,
	HMessageBoxResult_Cancel
}HMessageBoxResult;

typedef enum HMessageBoxType_H_
{
	HMessageBoxType_Warn,
	HMessageBoxType_Show
}HMessageBoxType;

#endif
