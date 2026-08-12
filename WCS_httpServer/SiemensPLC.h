// ============================================================================
// SiemensPLC.h — S7 PLC 通信封装（基于 Snap7 库）
//
// 职责：
//   ① 通过 S7 协议 (ISO-on-TCP, port 102) 直连 Siemens PLC
//   ② 读写 PLC Data Block (DBRead/DBWrite)
//   ③ 写入EPC编码+格口分拣指令到 DB1
//
// 参考: WCSApp\WCSApps\SiemensPLC.h
// ============================================================================

#pragma once

#include <vector>
#include <QByteArray>
#include <QString>
#include "define.h"
#include "snap7.h"

class CSiemensPLC
{
public:
    CSiemensPLC();
    ~CSiemensPLC();

    // 连接 PLC (rack=0, slot=1)
    bool connectTo(const char* ip);

    // 断开连接
    void disconnect();

    // 检查连接状态
    bool isConnected() const;

    // 读取 DB 块数据
    bool readData(int dbNum, int startAddr, int readSize, byte* outBuffer);

    // 写入 DB 块数据
    bool writeData(int dbNum, int startAddr, int size, const byte* data);

    // 写入EPC编码+格口信息到 DB1 (与 WCSApp 完全兼容)
    // 数据格式: DB1 Offset 1000, 42 bytes
    bool writeCodeInfo(const QByteArray& sSend, const std::vector<int>& vecGrid);

    // 获取最后一次错误信息
    int lastError() const;
    QString lastErrorText() const;

private:
    TS7Client* m_pClient;
};