
#ifndef __HLOG1_H__
#define __HLOG1_H__

#include "hlogconfig.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef void (HLOG_API* HLOG_CB)(HLOG_LEVEL level, const char* message);

    /** @fn int hlog_init(const char* name, char async = false)
     *  @brief 兼容旧接口, 不执行任何操作
     *  @return HLOG_OK
     */
#ifdef __cplusplus
    HLOG_DECLARE int HLOG_API hlog_init(const char* name, char async = false);
#else
    HLOG_DECLARE int HLOG_API hlog_init(const char* name, char async);
#endif

    /** @fn int HLOG_API hlog_ocx_init(const char* name, const char* path, char async = false)
     *  @brief 兼容旧接口, 不执行任何操作
     *  @return HLOG_OK
     */
#ifdef __cplusplus
    HLOG_DECLARE int HLOG_API hlog_ocx_init(const char* name, const char* path, char async = false);
#else
    HLOG_DECLARE int HLOG_API hlog_ocx_init(const char* name, const char* path, char async);
#endif

    /** @fn int hlog_fini()
     *  @brief 兼容旧接口, 不执行任何操作
     *  @return HLOG_OK
     */
    HLOG_DECLARE int HLOG_API hlog_fini();

    /** @fn int hlog_set_callback(HLOG_CB callback)
     *  @brief 设置日志的回调函数
     *  @param[in] callback 用于处理日志信息的回调函数
     *  @return HLOG_OK
     */
    HLOG_DECLARE int HLOG_API hlog_set_callback(HLOG_CB callback);

    /** @fn int hlog_level(HLOG_LEVEL level)
     *  @brief 兼容旧接口, 不执行任何操作
     *  @return HLOG_OK
     */
    HLOG_DECLARE int HLOG_API hlog_level(HLOG_LEVEL level);

    /** @fn int HLOG_API hlog_format(HLOG_LEVEL level, const char* module, const char* format, ...)
     *  @brief 写日志格式化数据
     *  @param[in] level 日志级别
     *  @param[in] module 日志模块
     *  @param[in] format 日志格式化字符串, 后接可变参数
     *  @return HLOG_OK, 成功; 否则, 失败
     *  @note hlog_format会调用hlog_set_callback传入的回调函数
     *  @warning 使用者必须保证输入的格式化字符串和可变参数相匹配
     *  @sa hlog_format_ex
     */
    HLOG_DECLARE int HLOG_API hlog_format(HLOG_LEVEL level, const char* module, const char* format, ...) LOG_CHECK_FMT(3, 4);

    /** @fn int HLOG_API hlog_format(int level, const char* module, const char* format, ...)
     *
     *  @brief 写日志格式化数据, 功能和hlog_format相同，只是接口参数定义更加通用
     *  @param[in] level 日志级别
     *  @param[in] module 日志模块
     *  @param[in] format 日志格式化字符串, 后接可变参数
     *  @return HLOG_OK, 成功; 否则, 失败
     *  @note hlog_format会调用hlog_set_callback传入的回调函数
     *  @warning 使用者必须保证输入的格式化字符串和可变参数相匹配
     *  @sa hlog_format
     */
    HLOG_DECLARE int HLOG_API hlog_format_ex(int level, const char* module, const char* format, ...) LOG_CHECK_FMT(3, 4);

    /** @fn int hlog_message(HLOG_LEVEL level, const char* module, const char* message)
     *  @brief 写日志字符串
     *  @param[in] level 日志级别
     *  @param[in] module 日志模块
     *  @param[in] message 日志字符串
     *  @return HLOG_OK, 成功; 否则, 失败
     *  @note hlog_message不会调用hlog_set_callback传入的回调函数
     *  @warning 在使用该接口时, 不应在message中携带%n等格式化输出的字符,
     *           否则可能有崩溃的风险, 如果不能确定输入的内容, 那么请使用
     *           hlog_message_ex接口
     *  @sa hlog_message_ex
     */
    HLOG_DECLARE int HLOG_API hlog_message(HLOG_LEVEL level, const char* module, const char* message);

    /** @fn int hlog_message_ex(HLOG_LEVEL level, const char* module, const char* message)
     *  @brief 写日志字符串
     *  @param[in] level 日志级别
     *  @param[in] module 日志模块
     *  @param[in] message 日志字符串
     *  @return HLOG_OK, 成功; 否则, 失败
     *  @sa hlog_message
     */
    HLOG_DECLARE int HLOG_API hlog_message_ex(HLOG_LEVEL level, const char* module, const char* message);

    /** @fn void HLOG_API hlog_set_configfile_name(const char* config_file_name)
     *  @brief 多进程共享一个配置文件时会出现日志文件滚动错误, 为了规避这一错误,
     *         特加上此接口, 不建议用这个接口, 如果仅是一个进程, 则最好用一个配
     *         置文件
     *  @param[in] config_file_name 配置文件名
     */
    HLOG_DECLARE void HLOG_API hlog_set_configfile_name(const char* config_file_name);

    /** @fn void HLOG_API hlog_set_immediate_flush(int immediate_flush);
     *
     *  @brief      设置是否立即刷写日志到文件（接口预留，暂未实现）
     *  @param[in]  immediate_flush, 是否立即刷写日志到文件, 0-false, else-true
     *  @warning    如果在日志配置文件中启用了日志缓冲区，而又在退出时段比较关注
     *              日志的即时性，那么可以考虑使用该接口控制进程回到立即写日志的
     *              状态
     *  @sa         hlog_flush_and_finalize
     */
    HLOG_DECLARE void HLOG_API hlog_set_immediate_flush(int immediate_flush);

    /** @fn void HLOG_API hlog_flush_and_finalize();
     *
     *  @brief      刷写缓存中最后的日志并反初始化日志库
     *  @warning    如果在日志配置文件中启用了日志缓冲区，那么必须在程序退出前
     *              调用该接口将缓冲区的日志刷写到日志文件中
     *  @sa         hlog_set_immediate_flush
     */
    HLOG_DECLARE void HLOG_API hlog_flush_and_finalize();

#ifdef __cplusplus
}
#endif

// 考虑兼容性，特增加log_printf_def和log_printf_dst
#if (defined _WIN32 || defined _WIN64)
#define log_printf_def(level, format, ...)      hlog_format(level, "", format, ##__VA_ARGS__)
#else
#define log_printf_def(level, format, args...)  hlog_format(level, "", format, ##args)
#endif
#define log_printf_dst hlog_format

// for defualt module
#if (defined _WIN32 || defined _WIN64)
#define LOG_ERROR(fmt, ...)         hlog_format(HLOG_LEVEL_ERROR, "", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)          hlog_format(HLOG_LEVEL_WARN, "", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define LOG_INFO(fmt, ...)              hlog_format(HLOG_LEVEL_INFO, "", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define LOG_DEBUG(fmt, ...)         hlog_format(HLOG_LEVEL_DEBUG, "", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define LOG_TRACE(fmt, ...)         hlog_format(HLOG_LEVEL_TRACE, "", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__) 

#else
#define LOG_ERROR(fmt, args...)     hlog_format(HLOG_LEVEL_ERROR, "", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define LOG_WARN(fmt, args...)      hlog_format(HLOG_LEVEL_WARN, "", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define LOG_INFO(fmt, args...)      hlog_format(HLOG_LEVEL_INFO, "", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define LOG_DEBUG(fmt, args...)     hlog_format(HLOG_LEVEL_DEBUG, "", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define LOG_TRACE(fmt, args...)     hlog_format(HLOG_LEVEL_TRACE, "", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#endif

// for HPP module
#if (defined _WIN32 || defined _WIN64)
#define HPP_ERROR(fmt, ...)         hlog_format(HLOG_LEVEL_ERROR, "HPP", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define HPP_WARN(fmt, ...)          hlog_format(HLOG_LEVEL_WARN, "HPP", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define HPP_INFO(fmt, ...)          hlog_format(HLOG_LEVEL_INFO, "HPP", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define HPP_DEBUG(fmt, ...)         hlog_format(HLOG_LEVEL_DEBUG, "HPP", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define HPP_TRACE(fmt, ...)         hlog_format(HLOG_LEVEL_TRACE, "HPP", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#else
#define HPP_ERROR(fmt, args...)     hlog_format(HLOG_LEVEL_ERROR, "HPP", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define HPP_WARN(fmt, args...)      hlog_format(HLOG_LEVEL_WARN, "HPP", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define HPP_INFO(fmt, args...)      hlog_format(HLOG_LEVEL_INFO, "HPP", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define HPP_DEBUG(fmt, args...)     hlog_format(HLOG_LEVEL_DEBUG, "HPP", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define HPP_TRACE(fmt, args...)     hlog_format(HLOG_LEVEL_TRACE, "HPP", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#endif

// for MQ module
#if (defined _WIN32 || defined _WIN64)
#define MQ_ERROR(fmt, ...)          hlog_format(HLOG_LEVEL_ERROR, "MQ", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define MQ_WARN(fmt, ...)           hlog_format(HLOG_LEVEL_WARN, "MQ", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define MQ_INFO(fmt, ...)           hlog_format(HLOG_LEVEL_INFO, "MQ", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define MQ_DEBUG(fmt, ...)          hlog_format(HLOG_LEVEL_DEBUG, "MQ", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define MQ_TRACE(fmt, ...)          hlog_format(HLOG_LEVEL_TRACE, "MQ", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#else
#define MQ_ERROR(fmt, args...)      hlog_format(HLOG_LEVEL_ERROR, "MQ", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define MQ_WARN(fmt, args...)       hlog_format(HLOG_LEVEL_WARN, "MQ", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define MQ_INFO(fmt, args...)       hlog_format(HLOG_LEVEL_INFO, "MQ", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define MQ_DEBUG(fmt, args...)      hlog_format(HLOG_LEVEL_DEBUG, "MQ", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define MQ_TRACE(fmt, args...)      hlog_format(HLOG_LEVEL_TRACE, "MQ", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#endif


// for CheckResultLog module
#if (defined _WIN32 || defined _WIN64)
#define WCS_ERROR(fmt, ...)         hlog_format(HLOG_LEVEL_ERROR, "WCS", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define WCS_WARN(fmt, ...)          hlog_format(HLOG_LEVEL_WARN, "WCS", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define WCS_INFO(fmt, ...)          hlog_format(HLOG_LEVEL_INFO, "WCS","\t" fmt, ##__VA_ARGS__)    
#define WCS_DEBUG(fmt, ...)         hlog_format(HLOG_LEVEL_DEBUG, "WCS", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define WCS_TRACE(fmt, ...)         hlog_format(HLOG_LEVEL_TRACE, "WCS", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#else
#define WCS_ERROR(fmt, args...)     hlog_format(HLOG_LEVEL_ERROR, "WCS", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define WCS_WARN(fmt, args...)      hlog_format(HLOG_LEVEL_WARN, "WCS", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define WCS_INFO(fmt, args...)      hlog_format(HLOG_LEVEL_INFO, "WCS", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define WCS_DEBUG(fmt, args...)     hlog_format(HLOG_LEVEL_DEBUG, "WCS", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define WCS_TRACE(fmt, args...)     hlog_format(HLOG_LEVEL_TRACE, "WCS", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#endif

// for CheckResultLog module
#if (defined _WIN32 || defined _WIN64)
#define JT_ERROR(fmt, ...)         hlog_format(HLOG_LEVEL_ERROR, "JT", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define JT_WARN(fmt, ...)          hlog_format(HLOG_LEVEL_WARN, "JT", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define JT_INFO(fmt, ...)          hlog_format(HLOG_LEVEL_INFO, "JT","\t" fmt, ##__VA_ARGS__)    
#define JT_DEBUG(fmt, ...)         hlog_format(HLOG_LEVEL_DEBUG, "JT", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define JT_TRACE(fmt, ...)         hlog_format(HLOG_LEVEL_TRACE, "JT", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#else
#define JT_ERROR(fmt, args...)     hlog_format(HLOG_LEVEL_ERROR, "JT", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define JT_WARN(fmt, args...)      hlog_format(HLOG_LEVEL_WARN, "JT", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define JT_INFO(fmt, args...)      hlog_format(HLOG_LEVEL_INFO, "JT", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define JT_DEBUG(fmt, args...)     hlog_format(HLOG_LEVEL_DEBUG, "JT", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define JT_TRACE(fmt, args...)     hlog_format(HLOG_LEVEL_TRACE, "JT", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#endif


// for Arith
#if (defined _WIN32 || defined _WIN64)
#define Arith_2D_ERROR(fmt, ...)         hlog_format(HLOG_LEVEL_ERROR, "2D", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define Arith_2D_WARN(fmt, ...)          hlog_format(HLOG_LEVEL_WARN, "2D", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define Arith_2D_INFO(fmt, ...)          hlog_format(HLOG_LEVEL_INFO, "2D","\t" fmt, ##__VA_ARGS__)    
#define Arith_2D_DEBUG(fmt, ...)         hlog_format(HLOG_LEVEL_DEBUG, "2D", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define Arith_2D_TRACE(fmt, ...)         hlog_format(HLOG_LEVEL_TRACE, "2D", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#else
#define Arith_2D_ERROR(fmt, args...)     hlog_format(HLOG_LEVEL_ERROR, "2D", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define Arith_2D_WARN(fmt, args...)      hlog_format(HLOG_LEVEL_WARN, "2D", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define Arith_2D_INFO(fmt, args...)      hlog_format(HLOG_LEVEL_INFO, "2D", "<%s>|<%d>\t<%s>," fmt,  __FILE__, __LINE__, __FUNCTION__, ##args)
#define Arith_2D_DEBUG(fmt, args...)     hlog_format(HLOG_LEVEL_DEBUG, "2D", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#define Arith_2D_TRACE(fmt, args...)     hlog_format(HLOG_LEVEL_TRACE, "2D", "<%s>|<%d>\t<%s>," fmt, __FILE__, __LINE__, __FUNCTION__, ##args)
#endif


#define PLC_ERROR(fmt, ...)         hlog_format(HLOG_LEVEL_ERROR, "PLC", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define PLC_WARN(fmt, ...)          hlog_format(HLOG_LEVEL_WARN, "PLC", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define PLC_INFO(fmt, ...)          hlog_format(HLOG_LEVEL_INFO, "PLC","\t" fmt, ##__VA_ARGS__)    
#define PLC_DEBUG(fmt, ...)         hlog_format(HLOG_LEVEL_DEBUG, "PLC", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define PLC_TRACE(fmt, ...)         hlog_format(HLOG_LEVEL_TRACE, "PLC", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    

#define Data_ERROR(fmt, ...)         hlog_format(HLOG_LEVEL_ERROR, "DataBase", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define Data_WARN(fmt, ...)          hlog_format(HLOG_LEVEL_WARN, "DataBase", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define Data_INFO(fmt, ...)          hlog_format(HLOG_LEVEL_INFO, "DataBase","\t" fmt, ##__VA_ARGS__)    
#define Data_DEBUG(fmt, ...)         hlog_format(HLOG_LEVEL_DEBUG, "DataBase", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define Data_TRACE(fmt, ...)         hlog_format(HLOG_LEVEL_TRACE, "DataBase", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)   

#define work2_ERROR(fmt, ...)         hlog_format(HLOG_LEVEL_ERROR, "work2", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define work2_WARN(fmt, ...)          hlog_format(HLOG_LEVEL_WARN, "work2", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define work2_INFO(fmt, ...)          hlog_format(HLOG_LEVEL_INFO, "work2","\t" fmt, ##__VA_ARGS__)    
#define work2_DEBUG(fmt, ...)         hlog_format(HLOG_LEVEL_DEBUG, "work2", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define work2_TRACE(fmt, ...)         hlog_format(HLOG_LEVEL_TRACE, "work2", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__) 

#define work3_ERROR(fmt, ...)         hlog_format(HLOG_LEVEL_ERROR, "work3", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define work3_WARN(fmt, ...)          hlog_format(HLOG_LEVEL_WARN, "work3", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define work3_INFO(fmt, ...)          hlog_format(HLOG_LEVEL_INFO, "work3","\t" fmt, ##__VA_ARGS__)    
#define work3_DEBUG(fmt, ...)         hlog_format(HLOG_LEVEL_DEBUG, "work3", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define work3_TRACE(fmt, ...)         hlog_format(HLOG_LEVEL_TRACE, "work3", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__) 


#define ImageSave_ERROR(fmt, ...)         hlog_format(HLOG_LEVEL_ERROR, "Image", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define ImageSave_WARN(fmt, ...)          hlog_format(HLOG_LEVEL_WARN, "Image", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define ImageSave_INFO(fmt, ...)          hlog_format(HLOG_LEVEL_INFO, "Image","\t" fmt, ##__VA_ARGS__)    
#define ImageSave_DEBUG(fmt, ...)         hlog_format(HLOG_LEVEL_DEBUG, "Image", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define ImageSave_TRACE(fmt, ...)         hlog_format(HLOG_LEVEL_TRACE, "Image", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    


#define PLCWarn_ERROR(fmt, ...)         hlog_format(HLOG_LEVEL_ERROR, "PLCWarn", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define PLCWarn_WARN(fmt, ...)          hlog_format(HLOG_LEVEL_WARN, "PLCWarn", "<%d>\t<%s>," fmt,  __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define PLCWarn_INFO(fmt, ...)          hlog_format(HLOG_LEVEL_INFO, "PLCWarn","\t" fmt, ##__VA_ARGS__)    
#define PLCWarn_DEBUG(fmt, ...)         hlog_format(HLOG_LEVEL_DEBUG, "PLCWarn", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    
#define PLCWarn_TRACE(fmt, ...)         hlog_format(HLOG_LEVEL_TRACE, "PLCWarn", "<%d>\t<%s>," fmt, __LINE__, __FUNCTION__, ##__VA_ARGS__)    


#endif
