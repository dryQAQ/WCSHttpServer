// ============================================================================
// SiemensPLC.cpp — S7 PLC 通信封装实现
//
// 参考: WCSApp\WCSApps\SiemensPLC.cpp
// ============================================================================

#include "SiemensPLC.h"
#include "LifecycleLogger.h"

CSiemensPLC::CSiemensPLC()
{
    m_pClient = new TS7Client();
}

CSiemensPLC::~CSiemensPLC()
{
    if (nullptr != m_pClient)
    {
        m_pClient->Disconnect();
        delete m_pClient;
        m_pClient = nullptr;
    }
}

bool CSiemensPLC::connectTo(const char* ip)
{
    if (!m_pClient) return false;

    int ret = m_pClient->ConnectTo(ip, PLC_S7_RACK, PLC_S7_SLOT);
    if (ret != 0)
    {
        PLC_LOG_ERROR("S7连接失败 ip=%s err=%s", ip, m_pClient->err_(ret).c_str());
        return false;
    }

    PLC_LOG_INFO("S7连接成功 ip=%s", ip);
    return true;
}

void CSiemensPLC::disconnect()
{
    if (m_pClient)
    {
        m_pClient->Disconnect();
        PLC_LOG_INFO("S7连接已断开");
    }
}

bool CSiemensPLC::isConnected() const
{
    return m_pClient ? m_pClient->Connected() : false;
}

bool CSiemensPLC::readData(int dbNum, int startAddr, int readSize, byte* outBuffer)
{
    if (!m_pClient || !m_pClient->Connected())
    {
        PLC_LOG_WARN("S7读取失败: 未连接 db=%d addr=%d", dbNum, startAddr);
        return false;
    }

    int ret = m_pClient->DBRead(dbNum, startAddr, readSize, outBuffer);
    if (ret != 0)
    {
        PLC_LOG_ERROR("S7 DBRead失败 db=%d addr=%d size=%d err=%s",
            dbNum, startAddr, readSize, m_pClient->err_(ret).c_str());
        return false;
    }
    return true;
}

bool CSiemensPLC::writeData(int dbNum, int startAddr, int size, const byte* data)
{
    if (!m_pClient || !m_pClient->Connected())
    {
        PLC_LOG_WARN("S7写入失败: 未连接 db=%d addr=%d", dbNum, startAddr);
        return false;
    }

    int ret = m_pClient->DBWrite(dbNum, startAddr, size, (void*)data);
    if (ret != 0)
    {
        PLC_LOG_ERROR("S7 DBWrite失败 db=%d addr=%d size=%d err=%s",
            dbNum, startAddr, size, m_pClient->err_(ret).c_str());
        return false;
    }
    return true;
}

bool CSiemensPLC::writeCodeInfo(const QByteArray& sSend, const std::vector<int>& vecGrid)
{
    if (!m_pClient || !m_pClient->Connected())
    {
        PLC_LOG_WARN("S7 writeCodeInfo失败: 未连接");
        return false;
    }

    // 构建 42 字节数据包（与 WCSApp 完全兼容）
    // DB1 Offset 1000
    QByteArray dataSend(PLC_S7_DB_WRITE_SIZE, 0x00);
    dataSend[0] = 0x7B; // '{'
    dataSend[1] = 0x52; // 'R'
    dataSend[2] = 0x56; // 'V'
    dataSend[3] = 0x2A; // '*'
    dataSend[4] = 0x7C; // '|'
    dataSend[5] = 0x01; dataSend[6] = 0x7C; dataSend[7] = 0x02;
    dataSend[8] = 0x7C; dataSend[9] = 0x12;

    // 条码数据（ASCII，最多 PLC_S7_CODE_MAX_LEN 字节，从 PLC_S7_CODE_OFFSET 开始写入）
    for (int i = 0; i < qMin(sSend.length(), PLC_S7_CODE_MAX_LEN); i++)
    {
        dataSend[i + PLC_S7_CODE_OFFSET] = sSend[i];
    }

    dataSend[35] = 0x7C; // '|'
    dataSend[36] = (uchar)(0x000000FF & vecGrid.size());
    dataSend[37] = (uchar)(0x000000FF & (vecGrid.size() > 0 ? vecGrid[0] : 0));
    dataSend[38] = (uchar)(0x000000FF & (vecGrid.size() > 1 ? vecGrid[1] : 0));
    dataSend[39] = (uchar)(0x000000FF & (vecGrid.size() > 2 ? vecGrid[2] : 0));
    dataSend[40] = (uchar)(0x000000FF & (vecGrid.size() > 3 ? vecGrid[3] : 0));
    dataSend[41] = 0x7D; // '}'

    int ret = m_pClient->DBWrite(PLC_S7_DB_WRITE, PLC_S7_DB_WRITE_OFFSET, PLC_S7_DB_WRITE_SIZE, (void*)dataSend.data());
    if (ret != 0)
    {
        PLC_LOG_ERROR("S7 writeCodeInfo失败 err=%s", m_pClient->err_(ret).c_str());
        return false;
    }
    return true;
}

int CSiemensPLC::lastError() const
{
    return m_pClient ? m_pClient->LastError() : -1;
}

QString CSiemensPLC::lastErrorText() const
{
    if (!m_pClient) return QString("null");
    int err = m_pClient->LastError();
    return QString::fromStdString(m_pClient->err_(err));
}