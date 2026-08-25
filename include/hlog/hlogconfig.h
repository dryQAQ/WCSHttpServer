
#ifndef __HLOGCONFIG_H__
#define __HLOGCONFIG_H__

#ifndef NULL
#define NULL 0
#endif // NULL

#if defined(_MSC_VER) // ʱ������
    typedef signed __int64	EVENT_TIME;
#elif defined(__GNUC__) || defined(__SYMBIAN32__)
#   if defined(__LP64__)
        typedef signed long EVENT_TIME;
#   else
        typedef signed long long EVENT_TIME;
#   endif	//defined(__LP64__)
#endif

#if (defined _WIN32 || defined _WIN64)
#   ifdef HLOG_EXPORTS
#       define HLOG_DECLARE __declspec(dllexport)
#   else
#       define HLOG_DECLARE __declspec(dllimport)
#   endif
#   define HLOG_API __stdcall
#else
#   ifdef __linux__
#       define HLOG_DECLARE 
#   else
#       define HLOG_DECLARE
#   endif
#   define HLOG_API
#endif

/** @enum HLOG_LEVEL
 *  @brief ��־����
 */
typedef enum {
    HLOG_LEVEL_OFF    = 7,
    HLOG_LEVEL_FATAL  = 6,
    HLOG_LEVEL_ERROR  = 5,
    HLOG_LEVEL_WARN   = 4,
    HLOG_LEVEL_INFO   = 3,
    HLOG_LEVEL_DEBUG  = 2,
    HLOG_LEVEL_TRACE  = 1,
    HLOG_LEVEL_ALL    = 0
} HLOG_LEVEL;

// �����붨�塣
#define HLOG_OK                             0 ///< �����ɹ���
#define HLOG_ERR                            -1 ///< ����ʧ�ܡ�
#define HLOG_BASE                           ((int)0x00000000)   ///< ���������
#define HLOG_E_InvalidParam                 (HLOG_BASE + 1) ///< �������Ϸ�
#define HLOG_E_OutOfMemory                  (HLOG_BASE + 2) ///< �����ڴ�ʧ��
#define HLOG_E_TaskQueueFail                (HLOG_BASE + 3) ///< ��������������ʧ��
#define HLOG_E_NULLPointer                  (HLOG_BASE + 4) ///< ��ָ��
#define HLOG_E_LevelNotEnable               (HLOG_BASE + 5) ///< ��־����û�дﵽ����ļ���
#define HLOG_E_CheckConfigTimer             (HLOG_BASE + 6) ///< �����������ļ��߳�ʧ��
#define HLOG_E_OpenLoggerModuleFail         (HLOG_BASE + 9) ///< ����־���ʧ��
#define HLOG_E_CreateThreadFail             (HLOG_BASE + 11) ///< �����߳�ʧ��
#define HLOG_E_ConfigFileNotExist           (HLOG_BASE + 13) ///< �����ļ�������
#define HLOG_E_BufferTooSmall               (HLOG_BASE + 16) ///< ��������С
#define HLOG_E_UnKnown                      (HLOG_BASE + 17) ///< δ֪����

// event error code
#define HLOG_E_DBOperateFail                (HLOG_BASE + 21) ///< db����ʧ��
#define HLOG_E_RecordEventFail              (HLOG_BASE + 22) ///< ��¼�¼�ʧ��

#define HLOG_IGNORE ""
#define HLOG_HANDLE void*

// ��־�궨�塣
#if defined(_MSC_VER)
#  define __HLOG_FILE__ __FILE__
#  define __HLOG_FUNC__ __FUNCSIG__
#  define __HLOG_LINE__ __LINE__
#elif defined(__GNUC__)
#  define __HLOG_FILE__ __BASE_FILE__
#  define __HLOG_FUNC__ __PRETTY_FUNCTION__
#  define __HLOG_LINE__ __LINE__
#else
#  define __HLOG_FILE__ ""
#  define __HLOG_FUNC__ ""
#  define __HLOG_LINE__ 0
#endif

#ifdef __GNUC__
#define LOG_CHECK_FMT(a, b) __attribute__((format(printf, a, b)))
#define CALLBACK
#else
#define LOG_CHECK_FMT(a, b)
#endif

#endif // __HLOGCONFIG_H__
