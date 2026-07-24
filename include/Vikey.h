#ifndef VIKEY_API
#define VIKEY_API

enum VikeyType
{
    ViKeyAPP = 0,						//实用型加密狗
    ViKeySTD = 1,						//标准型加密狗
    ViKeyNET = 2,						//网络型加密狗
    ViKeyPRO = 3,						//专业型加密狗
    ViKeyWEB = 4,						//身份认证型加密狗
    ViKeyTIME = 5,						//时钟型加密狗，内部独立时钟
    ViKeyMultiFunctional = 0x0A,		//多功能加密狗  支持软件加密 支持文档加密
    ViKeyMultiFunctionalTime = 0x0B,	//多功能时钟加密狗
    ViKeyInvalid	//无效类型
};


#define ViKeyNoLevel		0	//匿名权限(没有登录过加密狗时，此时权限为匿名权限)
#define ViKeyUserLevel		1	//用户权限(调用VikeyUserLogin登录加密狗成功后，此时权限为用户权限)
#define ViKeyAdminLevel		2	//管理员权限(调用VikeyAdminLogin登录加密狗成功后，此时权限为管理员权限)

#define VIKEY_SUCCESS						0x00000000 //成功
#define VIKEY_ERROR_NO_VIKEY				0x80000001 //没有找到ViKey加密锁
#define VIKEY_ERROR_INVALID_PASSWORD		0x80000002 //密码错误
#define VIKEY_ERROR_NEED_FIND				0x80000003 //请先查找加密锁
#define VIKEY_ERROR_INVALID_INDEX			0x80000004 //无效的句柄
#define VIKEY_ERROR_INVALID_VALUE			0x80000005 //数值错误
#define VIKEY_ERROR_INVALID_KEY				0x80000006 //秘钥无效
#define VIKEY_ERROR_GET_VALUE				0x80000007 //读取信息错误
#define VIKEY_ERROR_SET_VALUE				0x80000008 //设置信息错误
#define VIKEY_ERROR_NO_CHANCE				0x80000009 //没有机会
#define VIKEY_ERROR_NO_TAUTHORITY			0x8000000A //权限不足
#define VIKEY_ERROR_INVALID_ADDR_OR_SIZE	0x8000000B //地址或长度错误
#define VIKEY_ERROR_RANDOM					0x8000000C //获取随机数错误
#define VIKEY_ERROR_SEED					0x8000000D //获取种子错误
#define VIKEY_ERROR_CONNECTION				0x8000000E //通信错误
#define VIKEY_ERROR_CALCULATE				0x8000000F //算法或计算错误
#define VIKEY_ERROR_MODULE					0x80000010 //计数器错误
#define VIKEY_ERROR_GENERATE_NEW_PASSWORD	0x80000011 //产生密码错误
#define VIKEY_ERROR_ENCRYPT_FAILED			0x80000012 //加密数据错误
#define VIKEY_ERROR_DECRYPT_FAILED			0x80000013 //解密数据错误
#define VIKEY_ERROR_ALREADY_LOCKED			0x80000014 //ViKey加密锁已经被锁定
#define VIKEY_ERROR_UNKNOWN_COMMAND			0x80000015 //无效的命令
#define VIKEY_ERROR_NO_SUPPORT				0x80000016 //当前ViKey加密锁不支持此功能
#define VIKEY_ERROR_CATCH					0x80000017 //发生异常
#define VIKEY_ERROR_GET_USBDATA				0x80000018 //读取USB数据错误
#define VIKEY_ERROR_SET_USBDATA				0x80000019 //设置USB数据错误
#define VIKEY_ERROR_TIME_MODULE				0x8000001A //硬件时钟模块异常
#define VIKEY_ERROR_TIME_OUTAGE				0x8000001B //硬件时钟发生过断电，时钟可能不准确，需校准时钟
#define VIKEY_ERROR_UNKNOWN_ERROR			0xFFFFFFFF //未知错误

#ifdef __cplusplus
extern "C" {
#endif

    //【说明】为了让程序员对以下函数一目了然，这里只列出函数的原型，对于函数功能和参数的解释在帮助手册中列出。
    //【说明】请到开发包“1-[文档及视频教程]”目录下查看《ViKey系列加密狗使用说明手册》文档。

    /*********For ViKeyAPP、ViKeySTD、ViKeyNet、ViKeyPRO、ViKeyTime APIs**********/
    /*****************************************************************************/
    /*                                      API                                  */
    /*****************************************************************************/

    /**
     * @说明   查找加密锁。使用其他API前必须先调用次函数。
     * @param  pdwCount [out]     如果查找到系统中存在加密狗，返回查找到加密狗的个数
     * @return 0     表示系统中存在ViKey加密狗。
     */
    DWORD __stdcall VikeyFind(DWORD* pdwCount);

    /**
     * @说明   查找加密锁Ex。使用其他API前必须先调用次函数。
     * @param  pdwCount [out]     如果查找到系统中存在加密狗,返回查找到加密狗的个数（排除虚拟加密狗）
     * @return 0     表示系统中存在ViKey加密狗。
     */
    DWORD __stdcall VikeyFindEx(DWORD* pdwCount);

    /**
     * @说明   获取加密狗的硬件ID,加密狗的硬件ID是加密狗的唯一标识，每个加密狗的硬件ID都不一样。
     * @param  Index    [in]     指定要操作加密狗的序号
     * @param  pdwHID   [out]    返回加密狗的硬件ID
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyGetHID(WORD Index, DWORD* pdwHID);

    /**
     * @说明   获取加密狗的硬件型号。
     * @param  Index    [in]     指定要操作加密狗的序号
     * @param  pdwHID   [out]    返回加密狗的硬件型号。
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyGetType(WORD Index, VikeyType* pType);

    /**
     * @说明   获取加密狗的当前权限
     * @param  Index    [in]     指定要操作加密狗的序号
     * @param  pLevel   [out]    返回加密狗的当前权限 0表示加密狗尚未登录  1表示加密狗是用户权限  2表示加密狗是管理员权限
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyGetLevel(WORD Index, BYTE* pLevel);


    /**
     * @说明   设置机加密狗的产品名称
     * @param  Index    [in]     指定要操作加密狗的序号
     * @param  szName   [in]    16个字节的产品名称
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySetPtroductName(WORD Index, WCHAR szName[16]);

    /**
     * @说明   获取机加密狗的产品名称
     * @param  Index    [in]     指定要操作加密狗的序号
     * @param  szName   [out]    16个字节的产品名称
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyGetPtroductName(WORD Index, WCHAR szName[16]);
    DWORD __stdcall VikeyGetPtroductNameA(WORD Index, CHAR szName[16]);

    //登陆、注销加密狗
    //加密狗密码长度8个字符 字母或数字

    /**
     * @说明   以用户权限登录加密狗
     * @param  Index    [in]     指定要操作加密狗的序号
     * @param  szName   [in]     8字节的用户密码
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyUserLogin(WORD Index, char* pUserPassWord);

    /**
     * @说明   以管理员权限登录加密狗
     * @param  Index    [in]     指定要操作加密狗的序号
     * @param  szName   [in]     8字节的管理员密码
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyAdminLogin(WORD Index, char* pAdminPassWord);

    /**
     * @说明   注销登录加密狗 注销后的加密狗权限为0
     * @param  Index    [in]     指定要操作加密狗的序号
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyLogoff(WORD Index);

    //设置密码尝试次数、修改密码

    /**
     * @说明   设置用户权限密码最大错误尝试次数
     * @param  Index      [in]     指定要操作加密狗的序号
     * @param  cAttempt   [in]     用户权限密码最大错误尝试次数  如果设置为0表示无限制
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySetUserPassWordAttempt(WORD Index, BYTE cAttempt);

    /**
     * @说明   设置管理员权限密码最大错误尝试次数
     * @param  Index      [in]     指定要操作加密狗的序号
     * @param  cAttempt   [in]     管理员权限密码最大错误尝试次数  如果设置为0表示无限制
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySetAdminPassWordAttempt(WORD Index, BYTE cAttempt);

    /**
     * @说明   获取用户权限密码当前尝试次数和最大错误尝试次数
     * @param  Index              [in]      指定要操作加密狗的序号
     * @param  pcCurrentAttempt   [out]     用户权限密码当前误尝试次数
     * @param  pcMaxAttempt       [out]     用户权限密码最大错误尝试次数  如果设置为0表示无限制
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyGetUserPassWordAttempt(WORD Index, BYTE* pcCurrentAttempt, BYTE* pcMaxAttempt);


    /**
     * @说明   获取管理员权限密码当前尝试次数和最大错误尝试次数
     * @param  Index              [in]      指定要操作加密狗的序号
     * @param  pcCurrentAttempt   [out]     管理员权限密码当前误尝试次数
     * @param  pcMaxAttempt       [out]     管理员权限密码最大错误尝试次数  如果设置为0表示无限制
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyGetAdminPassWordAttempt(WORD Index, BYTE* pcCurrentAttempt, BYTE* pcMaxAttempt);

    /**
     * @说明   设置加密狗的用户权限密码和管理员权限密码  注意调用此函数时确保已经管理员权限登录  否则会调用失败
     * @param  Index              [in]     指定要操作加密狗的序号
     * @param  pNewUserPassWord   [in]     8字节的用户权限密码
     * @param  pNewAdminPassWord  [in]     8字节的管理员权限密码
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyResetPassword(WORD Index, char* pNewUserPassWord, char* pNewAdminPassWord);

    //软件ID读写
    //软件ID长度8个字符 字母或数字

    /**
     * @说明   设置加密狗的8个字节软件ID字符串
     * @param  Index              [in]     指定要操作加密狗的序号
     * @param  pSoftIDString      [in]     8个字节软件ID字符串
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySetSoftIDString(WORD Index, char* pSoftIDString);

    /**
     * @说明   获取加密狗的8个字节软件ID字符串
     * @param  Index              [in]     指定要操作加密狗的序号
     * @param  pSoftIDString      [out]    8个字节软件ID字符串
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyGetSoftIDString(WORD Index, char* pSoftIDString);

    //读写数据
    /**
     * @说明   读取加密狗中的数据
     * @param  Index       [in]     指定要操作加密狗的序号
     * @param  Addr        [in]     读取的地址 起始位置
     * @param  Length      [in]     读取的数据长度
     * @param  buffer      [out]    返回读取的数据内容
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyReadData(WORD Index, WORD Addr, WORD Length, BYTE* buffer);


    /**
     * @说明   向加密狗中的写入数据
     * @param  Index       [in]     指定要操作加密狗的序号
     * @param  Addr        [in]     读取的地址 起始位置
     * @param  Length      [in]     读取的数据长度
     * @param  buffer      [in]     写入的数据内容
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyWriteData(WORD Index, WORD Addr, WORD Length, BYTE* buffer);

    /**
     * @说明   从加密狗中的获取4个双字的随机数数据
     * @param  Index          [in]      指定要操作加密狗的序号
     * @param  pwRandom1      [out]     随机数1
     * @param  pwRandom2      [out]     随机数2
     * @param  pwRandom3      [out]     随机数3
     * @param  pwRandom4      [out]     随机数4
     * @return 0     表示操作成功
     */
    DWORD __stdcall ViKeyRandom(WORD Index, WORD* pwRandom1, WORD* pwRandom2, WORD* pwRandom3, WORD* pwRandom4);

    //计数器模块
    /**
     * @说明   设置加密狗中的计数器
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  wModuleIndex   [in]     计数器的序号 从0开始
     * @param  wValue         [in]     计数器中的初始值
     * @param  wMode          [in]     计数器的模式   1：允许递减  0：不允许递减
     * @return 0     表示操作成功
     */
    DWORD __stdcall ViKeySetModule(WORD Index, WORD wModuleIndex, WORD wValue, WORD wMode);

    /**
     * @说明   读取加密狗中的计数器
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  wModuleIndex   [in]     计数器的序号 从0开始
     * @param  pwValue        [out]    计数器中的当前值
     * @return 0     表示操作成功
     */
    DWORD __stdcall ViKeyGetModule(WORD Index, WORD wModuleIndex, WORD* pwValue);

    /**
     * @说明   对加密狗中的计数器进行递减操作
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  wModuleIndex   [in]     计数器的序号 从0开始
     * @return 0     表示操作成功
     */
    DWORD __stdcall ViKeyDecraseModule(WORD Index, WORD wModuleIndex);

    /**
     * @说明   对加密狗中的计数器进行检测
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  wModuleIndex   [in]     计数器的序号 从0开始
     * @param  IsZero         [out]    返回计数器中的当前值是否为0
     * @param  CanDecrase     [out]    返回计数器是否允许递减
     * @return 0     表示操作成功
     */
    DWORD __stdcall ViKeyCheckModule(WORD Index, WORD wModuleIndex, WORD* IsZero, WORD* CanDecrase);

    //加解密
    enum Des3KeyLengthType
    {
        Des3KeyLength16 = 0,
        Des3KeyLength24 = 1
    };
    /**
     * @说明   设置加密狗中3DES算法中的秘钥
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  pKey           [in]     3DES算法中的秘钥
     * @param  KeyType        [in]     3DES算法中的秘钥类型  0表示秘钥长度16字节 1表示秘钥长度24字节
     * @return 0     表示操作成功
     */
    DWORD __stdcall Vikey3DesSetKey(WORD Index, BYTE* pKey, Des3KeyLengthType KeyType);

    /**
     * @说明   执行加密狗中3DES算法中的加密操作
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  length         [in]     数据长度 明文长度必须为8的倍数
     * @param  pText          [in]     要加密的数据内容
     * @param  pResult        [out]    返回加密数据结果
     * @return 0     表示操作成功
     */
    DWORD __stdcall Vikey3DesEncrypt(WORD Index, WORD length, BYTE* pText, BYTE* pResult);

    /**
     * @说明   执行加密狗中3DES算法中的解密操作
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  length         [in]     数据长度 密文长度必须为8的倍数
     * @param  pText          [in]     要解密的数据内容
     * @param  pResult        [out]    返回解密数据结果
     * @return 0     表示操作成功
     */
    DWORD __stdcall Vikey3DesDecrypt(WORD Index, WORD length, BYTE* pText, BYTE* pResult);

    /**
     * @说明   设置加密狗中DES算法中的秘钥
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  pKey           [in]     DES算法中的秘钥 秘钥长度固定为8个字节
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyDesSetKey(WORD Index, BYTE* pKey);

    /**
     * @说明   执行加密狗中DES算法中的加密操作
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  length         [in]     数据长度 明文长度必须为8的倍数
     * @param  pText          [in]     要加密的数据内容
     * @param  pResult        [out]    返回加密数据结果
     * @param  OutLength      [out]    返回加密数据长度
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyDesEncrypt(WORD Index, WORD InLength, BYTE* pText, BYTE* pResult, WORD* OutLength);

    /**
     * @说明   执行加密狗中DES算法中的解密操作
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  length         [in]     数据长度 密文文长度必须为8的倍数
     * @param  pText          [in]     要解密的数据内容
     * @param  pResult        [out]    返回解密数据结果
     * @param  OutLength      [out]    返回解密数据长度
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyDesDecrypt(WORD Index, WORD InLength, BYTE* pText, BYTE* pResult, WORD* OutLength);

    //自动打开网页接口
    /**
     * @说明   设置加密狗中自动打开网址
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  pUrl           [in]     网址
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySetAutoRunUrl(WORD Index, BYTE* pUrl);

    /**
     * @说明   读取加密狗中自动打开网址
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  pUrl           [out]    网址
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyGetAutoRunUrl(WORD Index, BYTE* pUrl);

    //For ViKeyNET、ViKeyPRO
    /**
     * @说明   设置网络加密狗中最大链接的客户端数量
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  dwCount        [in]     最大链接的客户端数量
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySetMaxClientCount(WORD Index, WORD dwCount);

    /**
     * @说明   获取网络加密狗中最大链接的客户端数量
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  pdwCount       [out]    返回最大链接的客户端数量
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyGetMaxClientCount(WORD Index, WORD* pdwCount);

    //For ViKeyWEB、ViKeyPRO
    /**
     * @说明   执行加密狗中MD5算法
     * @param  Index         [in]     指定要操作加密狗的序号
     * @param  length        [in]     数据长度
     * @param  pText         [in]     数据内容
     * @param  pResult       [out]    返回MD5算法结果  16字节
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyMD5(WORD Index, WORD length, BYTE* pText, BYTE* pResult);

    /**
     * @说明   设置加密狗中HMAC-MD5算法的秘钥
     * @param  Index         [in]     指定要操作加密狗的序号
     * @param  pMD5key       [in]     秘钥内容 以0结尾
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySetMD5Key(WORD Index, BYTE* pMD5key);

    /**
     * @说明   执行加密狗中HMAC-MD5算法
     * @param  Index         [in]     指定要操作加密狗的序号
     * @param  length        [in]     数据长度
     * @param  pText         [in]     数据内容
     * @param  pResult       [out]    返回HMAC-MD5算法的加密结果
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyHmacMD5(WORD Index, WORD length, BYTE* pText, BYTE* pResult);

    /**
     * @说明   执行加密狗中SHA1算法
     * @param  Index         [in]     指定要操作加密狗的序号
     * @param  length        [in]     数据长度
     * @param  pText         [in]     数据内容
     * @param  pResult       [out]    返回SHA1算法结果  20字节
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySHA1(WORD Index, WORD length, BYTE* pText, BYTE* pResult);

    /**
     * @说明   设置加密狗中HMAC-SHA1算法的秘钥
     * @param  Index         [in]     指定要操作加密狗的序号
     * @param  pSHA1key      [in]     秘钥内容 以0结尾
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySetSHA1Key(WORD Index, BYTE* pSHA1key);

    /**
     * @说明   执行加密狗中HMAC-SHA1算法
     * @param  Index         [in]     指定要操作加密狗的序号
     * @param  length        [in]     数据长度
     * @param  pText         [in]     数据内容
     * @param  pResult       [out]    返回HMAC-SHA1算法的加密结果
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyHmacSHA1(WORD Index, WORD length, BYTE* pText, BYTE* pResult);

    /**
     * @说明   执行加密狗国秘SM3算法
     * @param  Index         [in]     指定要操作加密狗的序号
     * @param  length        [in]     输入数据的长度
     * @param  pText         [in]     输入数据的内容
     * @param  pResult       [out]    返回SM3算法结果  长度为32字节
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySM3(WORD Index, WORD length, BYTE* pText, BYTE* pResult);


    /**
     * @说明   设置加密狗中SM4算法中的秘钥
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  pKey           [in]     SM4算法中的秘钥 秘钥长度固定为16个字节
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySM4SetKey(WORD Index, BYTE* pKey);

    /**
     * @说明   执行加密狗中SM4算法中的加密操作
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  length         [in]     数据长度 长度必须为16的倍数
     * @param  pText          [in]     要加密的数据内容

     * @param  pResult        [out]    返回加密数据结果
     * @param  OutLength      [out]    返回加密数据长度
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySM4Encrypt(WORD Index, WORD InLength, BYTE* pText, BYTE* pResult, WORD* OutLength);

    /**
     * @说明   执行加密狗中SM4算法中的解密操作
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  length         [in]     密文数据长度 长度必须为16的倍数
     * @param  pText          [in]     要解密的密文数据内容

     * @param  pResult        [out]    返回解密数据结果
     * @param  OutLength      [out]    返回解密数据长度
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySM4Decrypt(WORD Index, WORD InLength, BYTE* pText, BYTE* pResult, WORD* OutLength);

    /**
     * @说明   执行非对称加密SM2算法的密钥对
     * @param  Index          [in]     指定要操作加密狗的序号

     * @param  pPrivateKey    [out]    返回私钥
     * @param  pPublicKey     [out]    返回公钥
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySM2CreateKey(WORD Index, BYTE* pPrivateKey, BYTE* pPublicKey);


    /**
     * @说明   SM2算法通过私钥计算公钥
     * @param  Index          [in]     指定要操作加密狗的序号
     * @param  pPrivateKey    [in]     输入私钥 长度为32字节

     * @param  pPublicKey     [out]    返回公钥
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySM2CalcPubKey(WORD Index, BYTE* pPrivateKey, BYTE* pPublicKey);

    /**
     * @说明   SM2算法通过私钥签名数据
     * @param  Index			[in]     指定要操作加密狗的序号
     * @param  pPrivateKey		[in]     输入私钥
     * @param  pUserID			[in]     输入用户ID
     * @param  wUserIDLength    [in]     输入用户ID长度
     * @param  pData			[in]     输入要签名的数据
     * @param  wDataLength		[in]     输入要签名的数据长度

     * @param  pSignR			[out]    返回签名R
     * @param  pSignS			[out]    返回签名S
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySM2Sign(WORD Index, BYTE* pPrivateKey, BYTE* pUserID, WORD wUserIDLength, BYTE* pData, WORD wDataLength, BYTE* pSignR, BYTE* pSignS);

    /**
     * @说明   SM2算法通过公钥验证签名
     * @param  Index			[in]     指定要操作加密狗的序号
     * @param  pPublicKey		[in]     输入公钥
     * @param  pUserID			[in]     输入用户ID
     * @param  wUserIDLength    [in]     输入用户ID长度
     * @param  pData			[in]     输入要验签的数据
     * @param  wDataLength		[in]     输入要验签的数据长度
     * @param  pSignR			[in]     输入签名R
     * @param  pSignS			[in]     输入签名S

     * @return 0     表示操作验证签名成功
     */
    DWORD __stdcall VikeySM2Verify(WORD Index, BYTE* pPublicKey, BYTE* pUserID, WORD wUserIDLength, BYTE* pData, WORD wDataLength, BYTE* pSignR, BYTE* pSignS);

    /**
     * @说明   SM2算法通过公钥进行数据加密
     * @param  Index			[in]     指定要操作加密狗的序号
     * @param  pPublicKey		[in]     输入公钥
     * @param  pData			[in]     输入加密的数据
     * @param  wDataLength		[in]     输入加密的数据长度

     * @param  pResult			[out]    输出加密后的结果密文
     * @param  wResultLength	[out]    输出加密后的结果密文长度

     * @return 0     表示操作加密成功
     */
    DWORD __stdcall VikeySM2Encrypt(WORD Index, BYTE* pPublicKey, BYTE* pData, WORD wDataLength, BYTE* pResult, WORD* wResultLength);

    /**
     * @说明   SM2算法通过私钥进行数据解密
     * @param  Index			[in]     指定要操作加密狗的序号
     * @param  pPrivateKey		[in]     输入私钥
     * @param  pData			[in]     输入解密的数据
     * @param  wDataLength		[in]     输入解密的数据长度

     * @param  pResult			[out]    输出解密后的结果明文
     * @param  wResultLength	[out]    输出解密后的结果明文长度

     * @return 0     表示操作解密成功
     */
    DWORD __stdcall VikeySM2Decrypt(WORD Index, BYTE* pPrivateKey, BYTE* pData, WORD wDataLength, BYTE* pResult, WORD* wResultLength);
    //For ViKeyTIME
    typedef struct _VIKEY_TIME
    {
        BYTE cYear;	      //年	
        BYTE cMonth;      //月
        BYTE cDay;        //日
        BYTE cHour;       //时
        BYTE cMinute;     //分
        BYTE cSecond;     //秒

        bool operator < (const _VIKEY_TIME& another) const;
        bool operator > (const _VIKEY_TIME& another) const;
    } SVikeyTime, * PVIKEYTIME;

    /**
     * @说明   获取时钟型加密狗中的内部独立时间
     * @param  Index         [in]     指定要操作加密狗的序号
     * @param  pTime         [out]    返回时钟型加密狗中的当前时间 6个字节 XX年XX月XX日XX时XX分XX秒
     * @return 0     表示操作成功
     */

    DWORD __stdcall VikeyGetTime(WORD Index, PVIKEYTIME pTime);

    /**
     * @说明   获取时钟型加密狗中的到期时间
     * @param  Index         [in]     指定要操作加密狗的序号
     * @param  pTime         [out]    返回时钟型加密狗中的到期时间 6个字节 XX年XX月XX日XX时XX分XX秒
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyGetValidTime(WORD Index, PVIKEYTIME pTime);

    /**
     * @说明   设置时钟型加密狗中的到期时间
     * @param  Index         [in]     指定要操作加密狗的序号
     * @param  pTime         [in]    返回时钟型加密狗中的到期时间 6个字节 XX年XX月XX日XX时XX分XX秒
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeySetValidTime(WORD Index, PVIKEYTIME pTime);

    /**
     * @说明   检测时钟型加密狗的时钟功能是否到期
     * @param  Index         [in]     指定要操作加密狗的序号
     * @param  pTime         [out]    返回是否到期结果 1表示没有到期   0表示已经到期
     * @return 0     表示操作成功
     */
    DWORD __stdcall VikeyCheckValidTime(WORD Index, BYTE* pIsValid);

#ifdef __cplusplus
} //  extern "C"{
#endif

#endif
