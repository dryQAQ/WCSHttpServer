# HP-Socket 学习指南 — 上午几小时快速上手

> 作者：WCS技术团队 | 版本：v1.0 | 日期：2026-06-02

---

## 目录

- [一、HP-Socket 简介](#一hp-socket-简介)
- [二、核心架构与组件](#二核心架构与组件)
- [三、项目中的实际应用案例](#三项目中的实际应用案例)
- [四、实践 Demo：TCP Client](#四实践-demo-tcp-client)
- [五、实践 Demo：TCP Server](#五实践-demo-tcp-server)
- [六、关键 API 详解](#六关键-api-详解)
- [七、常见问题与调试](#七常见问题与调试)

---

## 一、HP-Socket 简介

### 1.1 什么是 HP-Socket

**HP-Socket** 是一个高性能、跨平台的 TCP/UDP/HTTP 通信库，专为 Windows 平台设计，具有以下特点：

| 特性        | 说明                                  |
| --------- | ----------------------------------- |
| **高性能**   | 基于 IOCP（IO Completion Port）实现，支持高并发 |
| **异步非阻塞** | 事件驱动模型，避免线程阻塞                       |
| **易用性**   | 提供 C/C++ 接口，封装完善，使用简单               |
| **稳定性**   | 生产级稳定，广泛应用于金融、物流等领域                 |

### 1.2 项目中的应用场景

在 WCS 项目中，HP-Socket 主要用于：

| 场景        | 文件                                                           | 用途              |
| --------- | ------------------------------------------------------------ | --------------- |
| 圆通三段码查询   | [YT_Worker.cpp](file:///d:/WCS/WCSApp/WCSApps/YT_Worker.cpp) | TCP客户端连接圆通服务器   |
| 韵达 DES 通信 | [yunda_ems.cpp](file:///d:/WCS/WCSApp/WCSApps/yunda_ems.cpp) | TCP客户端连接韵达DES系统 |
| PLC 通信    | [PlcCenter.cpp](file:///d:/WCS/WCSApp/WCSApps/PlcCenter.cpp) | 多客户端管理          |

---

## 二、核心架构与组件

### 2.1 组件层次结构

```
┌─────────────────────────────────────────────────────────────┐
│                    HP-Socket 架构                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐ │
│  │  TCP Client  │    │  TCP Server  │    │  TCP Agent   │ │
│  │  (客户端)    │    │  (服务端)    │    │  (多客户端)  │ │
│  └──────┬───────┘    └──────┬───────┘    └──────┬───────┘ │
│         │                    │                    │         │
│         ▼                    ▼                    ▼         │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐ │
│  │   Listener   │    │   Listener   │    │   Listener   │ │
│  │  (回调接口)  │    │  (回调接口)  │    │  (回调接口)  │ │
│  └──────┬───────┘    └──────┬───────┘    └──────┬───────┘ │
│         │                    │                    │         │
│         └────────────────────┴────────────────────┘         │
│                              │                              │
│                              ▼                              │
│                    ┌──────────────┐                         │
│                    │   IOCP 层    │                         │
│                    │ (核心引擎)   │                         │
│                    └──────────────┘                         │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 核心组件说明

| 组件             | 接口                                          | 说明           |
| -------------- | ------------------------------------------- | ------------ |
| **TCP Client** | `ITcpClient`                                | 单个 TCP 客户端连接 |
| **TCP Server** | `ITcpServer`                                | TCP 服务端，监听端口 |
| **TCP Agent**  | `ITcpAgent`                                 | 管理多个客户端连接    |
| **Listener**   | `ITcpClientListener` / `ITcpServerListener` | 回调接口，处理事件    |

### 2.3 事件驱动模型

HP-Socket 使用**回调机制**处理网络事件：

```
连接建立 → OnConnect()
    ↓
数据到达 → OnReceive()
    ↓
发送完成 → OnSend()
    ↓
连接断开 → OnClose()
    ↓
错误发生 → OnError()
```

---

## 三、项目中的实际应用案例

### 3.1 圆通三段码查询 — TCP Client

**核心代码结构**（[YT_Worker.cpp](file:///d:/WCS/WCSApp/WCSApps/YT_Worker.cpp)）：

```cpp
class YT_Worker__Sanduanma : public ITcpClientListener
{
public:
    YT_Worker__Sanduanma() : m_tcp(this)  // 初始化智能指针，传入 listener
    {
        m_heartThread = std::thread(&YT_Worker__Sanduanma::OnHeartThread, this);
        m_threadParse = std::thread(&YT_Worker__Sanduanma::OnParseThread, this);
    }

    bool connect_server(WCSConf& conf) {
        std::string sip = conf.stGongPeiConfig.yt_ip.toStdString();
        bool bret = m_tcp->Start((TCHAR*)(sip.c_str()), 
                                 conf.stGongPeiConfig.yt_sanduanma_port, true);
        return bret;
    }

    // 实现 Listener 回调
    virtual EnHandleResult OnConnect(ITcpClient* pSender, CONNID dwConnID) override {
        LOG_INFO("YT connected");
        return HR_OK;
    }

    virtual EnHandleResult OnReceive(ITcpClient* pSender, CONNID dwConnID, 
                                     const BYTE* pData, int iLength) override {
        // 处理接收到的数据
        std::unique_lock<std::mutex> lock(m_lockParse);
        m_queueReulst.push(QByteArray((const char*)pData, iLength));
        return HR_OK;
    }

private:
    CTcpClientPtr m_tcp;  // HP-Socket 智能指针
    std::thread m_heartThread;  // 心跳线程
    std::thread m_threadParse;  // 解析线程
    std::queue<QByteArray> m_queueReulst;  // 数据队列
    std::mutex m_lockParse;  // 队列锁
};
```

### 3.2 韵达 DES 通信 — TCP Client

**核心代码结构**（[yunda_ems.cpp](file:///d:/WCS/WCSApp/WCSApps/yunda_ems.cpp)）：

```cpp
class DataRecvService_new : public ITcpClientListener
{
public:
    DataRecvService_new(QString sip, int port) : m_pTCP(this) {
        // 启动连接
        if (m_pTCP->Start((TCHAR*)m_sip.toStdString().c_str(), m_port)) {
            // 发送认证消息
            QByteArray dataSend;
            dataSend.push_back(0x02);  // STX
            QString sendMsg = "AUTH:SORT-1";
            dataSend.append(sendMsg.toLatin1());
            dataSend.push_back(0x03);  // ETX
            m_pTCP->Send((const BYTE*)dataSend.data(), dataSend.length());
        }
    }

    virtual EnHandleResult OnReceive(ITcpClient* pSender, CONNID dwConnID,
                                     const BYTE* pData, int iLength) override {
        // 解析 ST 消息：ST-1-2:0,3916328558682,1,0|90|120,...
        QByteArray qdata((const char*)pData, iLength);
        std::unique_lock<std::mutex> lock(m_lockRecvData);
        m_queueRecvData.push(qdata);
        return HR_OK;
    }

private:
    CTcpClientPtr m_pTCP;
    QString m_sip;
    int m_port;
    std::queue<QByteArray> m_queueRecvData;
};
```

---

## 四、实践 Demo：TCP Client

### 4.1 创建 TCP Client 步骤

```cpp
#include "HPSocket.h"
#include <iostream>
#include <string>

// 步骤1：继承 ITcpClientListener
class MyClientListener : public ITcpClientListener
{
public:
    // 连接成功回调
    virtual EnHandleResult OnConnect(ITcpClient* pSender, CONNID dwConnID) override {
        std::cout << "[Client] Connected to server" << std::endl;
        return HR_OK;
    }

    // 数据到达回调
    virtual EnHandleResult OnReceive(ITcpClient* pSender, CONNID dwConnID, 
                                     const BYTE* pData, int iLength) override {
        std::string data((const char*)pData, iLength);
        std::cout << "[Client] Received: " << data << std::endl;
        return HR_OK;
    }

    // 发送完成回调
    virtual EnHandleResult OnSend(ITcpClient* pSender, CONNID dwConnID, 
                                  const BYTE* pData, int iLength) override {
        std::cout << "[Client] Sent " << iLength << " bytes" << std::endl;
        return HR_OK;
    }

    // 连接关闭回调
    virtual EnHandleResult OnClose(ITcpClient* pSender, CONNID dwConnID, 
                                   EnSocketOperation enOperation, int iErrorCode) override {
        std::cout << "[Client] Disconnected" << std::endl;
        return HR_OK;
    }

    // 错误回调
    virtual EnHandleResult OnError(ITcpClient* pSender, CONNID dwConnID, 
                                   EnSocketError enError) override {
        std::cout << "[Client] Error: " << enError << std::endl;
        return HR_OK;
    }
};

// 步骤2：使用客户端
int main() {
    // 创建 Listener 和 Client
    MyClientListener listener;
    CTcpClientPtr client(&listener);

    // 步骤3：连接服务器
    const char* serverIP = "127.0.0.1";
    USHORT serverPort = 9999;

    if (!client->Start((TCHAR*)serverIP, serverPort, TRUE)) {
        std::cerr << "Connect failed!" << std::endl;
        return -1;
    }

    // 步骤4：发送数据
    std::string message = "Hello HP-Socket!";
    client->Send((const BYTE*)message.c_str(), message.length());

    // 保持连接
    std::cout << "Press Enter to exit..." << std::endl;
    std::cin.get();

    // 步骤5：关闭连接
    client->Stop();

    return 0;
}
```

### 4.2 编译配置

在 Visual Studio 中配置：

1. **包含目录**：`d:\WCS\WCSApp\inc\HPSocket`
2. **库目录**：`d:\WCS\WCSApp\lib\x64`（或 x86）
3. **链接器输入**：`HPSocket.lib`（Release）或 `HPSocket_D.lib`（Debug）
4. **预处理器定义**：`HPSOCKET_STATIC_LIB`（如果使用静态库）

---

## 五、实践 Demo：TCP Server

### 5.1 创建 TCP Server 步骤

```cpp
#include "HPSocket.h"
#include <iostream>
#include <map>

// 步骤1：继承 ITcpServerListener
class MyServerListener : public ITcpServerListener
{
public:
    // 新连接到达回调
    virtual EnHandleResult OnAccept(ITcpServer* pSender, CONNID dwConnID, 
                                    const SOCKADDR_IN* pRemoteAddr) override {
        char ip[20];
        USHORT port;
        SYS_GetSocketRemoteAddress((SOCKET)dwConnID, ip, 20, port);
        std::cout << "[Server] New connection from " << ip << ":" << port << std::endl;
        return HR_OK;
    }

    // 数据到达回调
    virtual EnHandleResult OnReceive(ITcpServer* pSender, CONNID dwConnID, 
                                     const BYTE* pData, int iLength) override {
        std::string data((const char*)pData, iLength);
        std::cout << "[Server] Received from " << dwConnID << ": " << data << std::endl;

        // 回复客户端
        std::string response = "Echo: " + data;
        pSender->Send(dwConnID, (const BYTE*)response.c_str(), response.length());

        return HR_OK;
    }

    // 连接关闭回调
    virtual EnHandleResult OnClose(ITcpServer* pSender, CONNID dwConnID, 
                                   EnSocketOperation enOperation, int iErrorCode) override {
        std::cout << "[Server] Connection " << dwConnID << " closed" << std::endl;
        return HR_OK;
    }

    // 错误回调
    virtual EnHandleResult OnError(ITcpServer* pSender, CONNID dwConnID, 
                                   EnSocketError enError) override {
        std::cout << "[Server] Error on " << dwConnID << ": " << enError << std::endl;
        return HR_OK;
    }
};

// 步骤2：使用服务器
int main() {
    // 创建 Listener 和 Server
    MyServerListener listener;
    CTcpServerPtr server(&listener);

    // 步骤3：启动服务器
    const char* ip = "0.0.0.0";  // 监听所有接口
    USHORT port = 9999;

    if (!server->Start((TCHAR*)ip, port)) {
        std::cerr << "Server start failed!" << std::endl;
        return -1;
    }

    std::cout << "[Server] Started on port " << port << std::endl;
    std::cout << "Press Enter to stop..." << std::endl;
    std::cin.get();

    // 步骤4：停止服务器
    server->Stop();
    std::cout << "[Server] Stopped" << std::endl;

    return 0;
}
```

---

## 六、关键 API 详解

### 6.1 Client 核心 API

| 方法                      | 说明                   |
| ----------------------- | -------------------- |
| `Start(ip, port, sync)` | 启动连接，sync=true表示同步连接 |
| `Stop()`                | 停止连接                 |
| `Send(data, length)`    | 发送数据                 |
| `IsConnected()`         | 检查连接状态               |

### 6.2 Server 核心 API

| 方法                           | 说明        |
| ---------------------------- | --------- |
| `Start(ip, port)`            | 启动监听      |
| `Stop()`                     | 停止监听      |
| `Send(connId, data, length)` | 向指定连接发送数据 |
| `Close(connId)`              | 关闭指定连接    |
| `GetConnectionCount()`       | 获取连接数     |

### 6.3 智能指针使用

HP-Socket 提供智能指针，自动管理生命周期：

```cpp
// 方式1：构造时传入 listener
CTcpClientPtr client(&listener);  // 自动调用 HP_Create_TcpClient

// 方式2：手动创建
ITcpClient* pClient = HP_Create_TcpClient(&listener);
// 使用完毕后手动销毁
HP_Destroy_TcpClient(pClient);
```

---

## 七、常见问题与调试

### 7.1 连接失败

**原因分析**：

- 服务器未启动或端口未开放
- 防火墙阻止连接
- IP/端口配置错误

**调试方法**：

```cpp
bool bret = client->Start((TCHAR*)ip, port, true);
if (!bret) {
    DWORD error = SYS_GetLastError();
    std::cout << "Connect failed, error: " << error << std::endl;
}
```

### 7.2 数据粘包问题

**解决方案**：定义协议帧格式

```cpp
// 方案1：长度前缀（如圆通使用8位数字长度）
QString length = QString("%1").arg(msg.length(), 8, 10, QChar('0'));
QString sendData = length + msg;

// 方案2：特殊字符分隔（如韵达使用 STX/ETX）
dataSend.push_back(0x02);  // STX
dataSend.append(message);
dataSend.push_back(0x03);  // ETX
```

### 7.3 线程安全

**注意事项**：

- `OnReceive` 在 IOCP 线程中执行，不要阻塞
- 使用队列 + 锁进行数据传递
- 避免在回调中执行耗时操作

```cpp
// 正确做法：回调中只做数据拷贝
virtual EnHandleResult OnReceive(...) override {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push(QByteArray((const char*)pData, iLength));
    return HR_OK;
}

// 在独立线程中处理
void ProcessThread() {
    while (running) {
        QByteArray data;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_queue.empty()) {
                data = m_queue.front();
                m_queue.pop();
            }
        }
        // 处理数据...
    }
}
```

---

## 八、进阶用法

### 8.1 TCP Agent（多客户端管理）

```cpp
// 创建 Agent 管理多个连接
CTcpAgentPtr agent(&listener);

// 添加连接
agent->AddConnection((TCHAR*)"192.168.1.100", 8080);
agent->AddConnection((TCHAR*)"192.168.1.101", 8080);

// 启动所有连接
agent->Start();

// 发送广播
agent->Send((const BYTE*)"Broadcast", 9);
```

### 8.2 HTTP 客户端

```cpp
CHttpClientPtr httpClient(&httpListener);
httpClient->Start();

// 发送 GET 请求
httpClient->Request((TCHAR*)"GET", (TCHAR*)"http://example.com/api", nullptr, 0);
```

---

## 九、参考资料

1. **官方文档**：[HP-Socket GitHub](https://github.com/ldcsaa/HP-Socket)
2. **项目示例**：[YT_Worker.cpp](file:///d:/WCS/WCSApp/WCSApps/YT_Worker.cpp)、[yunda_ems.cpp](file:///d:/WCS/WCSApp/WCSApps/yunda_ems.cpp)
3. **头文件**：[HPSocket.h](file:///d:/WCS/WCSApp/inc/HPSocket/HPSocket.h)

---

## 练习任务

1. ✅ 运行 TCP Client Demo，连接到测试服务器
2. ✅ 运行 TCP Server Demo，测试客户端连接
3. ✅ 实现一个简单的 Echo 客户端-服务器程序
4. ✅ 分析项目中圆通和韵达的 HP-Socket 使用差异
5. ✅ 尝试在现有代码中添加新的 TCP 连接功能

---

> **提示**：先从简单的 Demo 开始，熟悉基本用法后，再研究项目中的实际应用案例。重点关注回调机制和线程安全处理。
