<div align="center">

# WCSHttpServer · 窄带分拣控制系统

**把 WMS 的纸面计划，变成轨道上每一件货的物理动作 —— 并且每一次动作都可追溯、可补偿、可复盘。**

一套已在产线实际运行的 Windows 分拣控制软件（WCS）。
衔接 **WMS 任务计划** · **RFID 实物身份** · **PLC 物理执行** · **SQLite 落库留痕**。

[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D6?logo=windows&logoColor=white)](#-快速开始)
[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](#-快速开始)
[![Qt](https://img.shields.io/badge/Qt-5.15.2-41CD52?logo=qt&logoColor=white)](#-快速开始)
[![MSVC](https://img.shields.io/badge/MSVC-v142%20%28x64%29-5C2D91?logo=visualstudio&logoColor=white)](#-快速开始)
[![Protocol](https://img.shields.io/badge/protocol-HTTP%20%7C%20TCP%20%7C%20S7-success)](#-协议速查表h4h8)
[![Docs](https://img.shields.io/badge/docs-50%2B%20份现场文档-blue)](#-文档地图)
[![Tests](https://img.shields.io/badge/tests-16%20个单测%20%7C%2014%20步回归-orange)](#-测试与仿真)
[![Status](https://img.shields.io/badge/status-生产运行中-brightgreen)](#-生产就绪度与已知风险)
[![PRs](https://img.shields.io/badge/PRs-welcome-ff69b4)](#-参与贡献)

[项目简介](#-这是什么项目) · [快速开始](#-快速开始) · [核心难点](#-四个真正难的地方) · [架构总览](#-架构总览) ·
[协议速查](#-协议速查表h4h8) · [配置部署](#️-配置与部署) · [界面操作](#-界面与操作闭环) ·
[新手路线](#-新手学习路线) · [FAQ](#-常见问题-faq) · [文档地图](#-文档地图) · [参与贡献](#-参与贡献)

</div>

---

## 📖 这是什么项目

一句话：**WCS（Warehouse Control System，仓储控制系统）**。

WMS（仓储管理系统）负责"想"——哪个波次要分哪些货、每个 SKU 分到几号格口。
PLC 负责"做"——推、转、落格。
**WCS 是中间那个必须同时听懂三方的角色**：把计划翻译成设备指令、把设备动作翻译成业务事实、把事实翻译成对账单。

```text
    WMS 的「计划」  ─┐
                    ├──►  WCS：对齐、判定、下发、记账、补偿  ──►  PLC 的「动作」
    RFID 的「身份」─┘                                          └──►  WMS 的「确认」
```

这个仓库就是那个"中间角色"的完整实现，包含 **操作界面 + HTTP 接口 + RFID 接入 + PLC 通信 + 波次调度 + 断点恢复 + 失败补传**。

> **它不是什么**：不是通用 WMS，不是算法 Demo，不是框架。它是一个**为真实产线写死业务规则、并在真实产线上被问题打磨过 52 次提交**的现场系统。

### 为什么值得看

| 常见的"分拣 Demo" | 这个项目 |
| --- | --- |
| 收到条码 → 查表 → 输出格口号 | 信息**分批异步**到达，还要判断"这一件现在到底能不能动" |
| 一个 `while` 循环处理消息 | **5 类线程池 + 事件循环**，每条路径有明确的耗时预算 |
| 报错就 `log.error()` 然后丢弃 | **四层兜底 + 人工补偿入口 + 离线可复现仿真** |
| 重启后从头再来 | **断电兜底重建**，切回上一波次继续干 |
| 假设设备永远听话 | 锁格、满箱、换箱、人工硬塞、重复扫码**全都建模了** |

---

## ⚡ 30 秒看懂它有多难

一次分拣，从 RFID 读到货到 PLC 动起来，**现场要求 ≤ 1 秒**（`PLC_SEND_TIMEOUT_MS`）。
但在这 1 秒内，系统必须：

```text
RFID 读到 EPC
   │  ① EPC 得先做形态识别（"A + 23 位数字"，长度可配），从一堆噪声里抠出真码
   ▼
EPC → SKU 异步 HTTP 查询   ──── 可能超时、可能重试、可能比下一条还晚回来
   │  ② 查询没回来时不能干等，也不能乱发；要挂起并记住"我在等什么"
   ▼
EPC → 小车号（carNum）     ──── 从 RFID 帧的 SN 序号里推导，可能比 SKU 先到
   │  ③ 两个异步结果要汇合，谁先到都不能丢信息
   ▼
判定：这一件现在能发吗？
   │  ④ 当前波次对不对？SKU 在本波有计划格口吗？格口锁了吗？容器绑了吗？
   │     满箱后重绑了吗？这件是不是还在途（防重复指令）？是不是合法重投？
   ▼
选格：SKU 计划分布在多个格口
   │  ⑤ 按「每格口 已落 + 在途 ≤ 计划」额度分流，认领必须原子（防两人抢同一件额度）
   ▼
下发 PLC  {EPC|格口|小车}
   │  ⑥ 网络发送本身也可能超时
   ▼
PLC 落格反馈  {EPC|格口|小车|小车+2|状态}
   │  ⑦ 反馈是批量的、可能迟到、可能来自上一波次
   ▼
记账：解除在途 / 写实绩 / 更新进度 / 触发 H7 满箱回传
```

**每一层都可能失败，而失败不能丢数据。** 这就是整个项目的技术含量所在。

---

## 🧠 四个真正难的地方

### 1️⃣ 信息分批到达，却只能形成"一次"有效动作

RFID 先给 EPC 和小车号，SKU 查询稍后返回，而波次可能还没开始。
系统用 `EpcCache` 做**信息汇合**：谁先到先存着，其他人到了再合并，绝不因为"另一个还没来"就丢掉已获得的信息。

难点不在缓存，而在 **判断"何时可以推进"**：就绪条件、开工状态、是否在途，三者缺一不可。
以及——**切波次后，旧波次的迟到回调不能污染新波次**。

### 2️⃣ 同一件货第二次出现：是"重复读到"还是"人工返工"？

这是最容易被 Demo 忽略、却最容易被现场打脸的地方。

- 件还在轨道上被重复识别 → **必须抑制**，否则 PLC 收到两条指令，货掉错格口
- 操作员把已落格的件**人工重新上料** → **必须放行**，否则现场没法返工

系统用「在途状态 + 冷却时间 + 重投次数上限」把两者分开：

```text
rescanResendEnabled   = true     # 同波次重扫重投开关
rescanResendCooldownMs = 1000    # 冷却 1 秒内的重复识别视为噪声
rescanResendMaxTimes   = 3       # 最多允许 3 次合法重投
plcInFlightTimeoutMs   = 30000   # 30 秒没有落格反馈 → 在途超时，可重新发送
```

> 💡 用"永久拒绝重复 EPC"是最简单也最错的做法——它会让现场无法返工。

### 3️⃣ 账实必须守恒：每格口「已落 + 在途 ≤ 计划」

同一个 SKU 可能**同时分到分类格口和发货格口**（各有一份计划数量）。
现场会多投、会投错、会换箱后同一件被报两次。

`PlanAllocTable` 把计划在波次开始时**一次性编译**成定长数组，运行期只做整数加减：

```text
remain = plan - landed - reserv        ← 恒等式，任何时刻都必须成立

三个唯一写点（除此外没有任何地方能改额度）：
  claim()          下发 PLC 前认领额度（原子，防超投竞态）
  commitOnLanded() 收到落格反馈后提交实绩
  release()        异常/取消时归还

配套：
  audit()              每 30 秒巡检一次额度是否自洽
  sweepExpiredClaims() 认领超时（下发后无反馈）自动扫描清理
```

**为什么"一次性编译"很关键**：改造前选格依赖逗号串解析顺序和多次 `QMap` 查找，
结果会**随消息到达顺序漂移**——出了问题事后根本没法复现。
编译成定长数组后，分配结果与到达顺序无关，**可重现、可审计、可离线仿真**。

超计划怎么办？**不污染 WMS**，四层闭环改投物理异常口（66 号）：

```text
分配层：额度满 → 改投 66 号
反馈层：不计已分拣、不写明细
报文层：H7 数量按计划裁剪、异常口件不进 H7
明细层：不落库
```

少任何一层，WMS 就会按格口校验把**整条报驳**回来。

### 4️⃣ 意外关窗 / 断电，不能变成丢波次事故

```text
点「关闭窗口」 = 切出当前波次（绑定归档 + 清内存，进度/明细/报文全留 DB）
再次打开     = 一个新任务（不会莫名其妙接着上一波）
历史波次列表 = 可「切回」继续做（重建映射、额度、去重集、箱内明细）
终态波次     = 已完成/已取消 → 禁止切回（按钮直接置灰）
```

启动时还有**兜底恢复**：扫描数据库里"已落格但 H7 还没形成"的中间状态，
按 `sorting_records` 重建回箱内明细。窗口关闭还有 **120 秒绑定归属窗口**，
避免误关后绑定记录归属到错误的波次。

---

## 🏗 架构总览

```text
                     ┌─────────────────────── WCS_httpServer.exe（单进程）───────────────────────┐
                     │                                                                        │
   WMS ──H4/H5/H6──► │  ┌──────────────┐   查 SKU    ┌──────────────┐                        │
   (HTTP POST)       │  │  HttpServer  │◄───────────►│  HttpClient  │──H7 满箱 / H8 完结──► WMS │
     :8191           │  │  接入 + 编排  │             │  回传 + 查询  │        (HTTP/HTTPS)     │
                     │  └──────┬───────┘             └──────────────┘                        │
                     │         │                                                              │
                     │    ┌────▼─────┐   ┌──────────────┐   ┌───────────────┐                 │
                     │    │ WaveTask │──►│ ParseWorker  │──►│ PlanAllocTable│                 │
                     │    │ 有界队列  │   │  独立线程解析 │   │  计划分配表    │                 │
                     │    └──────────┘   └──────┬───────┘   └───────┬───────┘                 │
                     │                          │ WAVE_MAP 留痕      │ 额度认领/提交            │
                     │    ┌──────────────┐      ▼                   ▼                         │
   RFID ──EPC+小车──►│    │ RfidPushClient│  ┌─────────┐     ┌──────────────┐                 │
   (WCS 主动连出)     │    │  重连 + 心跳  │─►│ EpcCache │────►│  PlcManager  │──EPC|格口|小车──►│
     TCP :2010       │    └──────────────┘  │ 多源汇合  │     │  选格 + 通信  │◄─落格反馈────────│
                     │                      └─────────┘     └──────┬───────┘   TCP :8192      │
   PLC ◄──S7 锁格─────│                                            │                          │
   (Snap7 DB77 位图)  │                                     ┌──────▼───────┐                  │
                     │                                     │SortingDatabase│                 │
                     │                                     │ 专用写线程     │                 │
                     │                                     └──────┬───────┘                  │
                     │  ┌──────────────┐                           ▼                          │
                     │  │  MainWindow  │  ◄── 操作员              SQLite                     │
                     │  │  运行界面/面板 │                     data/sorting_records.db         │
                     │  └──────────────┘                                                      │
                     └────────────────────────────────────────────────────────────────────────┘
```

### 为什么这么拆线程

不是为了炫技，**每一条都是被"1 秒预算"逼出来的**：

| 执行域 | 规模 | 为什么必须独立 |
| --- | --- | --- |
| HTTP 业务池 | `businessPoolSize = 90` | 网络回调必须立刻交棒，不能在大波次解析里堵死 |
| PLC 发送池 | `plcSendPoolSize = 8` | 下发指令有 1 秒硬预算，不能被数据库拖慢 |
| PLC 反馈池 | `plcRecvPoolSize = 4` | 落格反馈丢失 = 账实不符，必须有独立车道 |
| 波次解析线程 | `ParseWorker`（独立 `QThread`） | 大波次解析（SKU×格口）会跑几百毫秒，绝不能占住 I/O 回调 |
| 数据库写线程 | `SortingDatabase` 专用写线程 | SQLite 写是串行的，把它从主线程赶走，UI 才不卡 |
| 主线程 | Qt 事件循环 | 只做界面与调度，**任何长任务都不能在这里跑** |

> 📌 设计原则：**按耗时特征安排执行位置**。网络接收、报文解析、设备发送、数据库操作分属不同执行域；
> 诊断留痕（`WAVE_MAP`）刻意放在解析线程，`rfid_raw` 批量落库放在定时器——**留痕绝不能拖慢分拣**。

---

## 🔌 协议速查表（H4~H8）

这是理解项目的**最快入口**。WMS 侧的接口在文档里叫 H1~H8，本项目涉及其中 5 个：

| 报文 | 方向 | 端点 | 作用 |
| :---: | --- | --- | --- |
| **H4** | WMS → WCS | `POST /api/DispatchSortingCommand/InsertWaveInfo` | **下发波次**：波次号 + SKU/格口计划数量 |
| **H5** | WMS → WCS | `POST /api/DispatchSortingCommand/InsertWaveIn` | **取消波次**（分拣开始后有状态限制，不是任意时刻可取消） |
| **H6** | WMS → WCS | `POST /api/DispatchSortingCommand/BindingLatticePort` | **绑定格口容器**：哪个箱号放在哪个格口 |
| **H7** | WCS → WMS | 网关 `gwisSubProductClassifyOrder` | **满箱回传**：该格口这一箱的分拣明细 |
| **H8** | WCS → WMS | 网关 `gwisSubProductClassifyEndOrder` | **波次完结回传**：本波整体结果 |

### 三条业务链路

```text
① 计划链路（HTTP，WMS 主动推）
   H4 波次 ──► 解析 ──► 编译计划分配表 ──► 等绑定 ──► 等操作员点「开始分拣」

② 执行链路（TCP + S7，设备交互）
   RFID 推 EPC ──► 查 SKU ──► 多源汇合 ──► 额度认领 ──► 下发 PLC ──► 落格反馈 ──► 写实绩

③ 确认链路（HTTP，WCS 主动推）
   满箱锁格(S7) / 换箱 ──► H7 满箱回传 ──► 失败自动重试 + 人工补传
   点「结束任务」      ──► 补发 → 固定延迟 2s → H8 完结回传（无论 H7 成败）
```

### 格口编码：三种写法都要认

现场同一件事有三套叫法，**归一化是必须的**：

```text
WMS 下发    22005      ← 前缀 22 + 宽度 3，可配置（gridCodePrefix / gridCodeWidth）
内部 key    005        ← 补零 3 位（GRID_KEY_PADDING = 3），所有存储/比较都用它
现场口述    5          ← 操作员在补传下拉框里可能手输这个

→ WmsGridCode.h 负责互相转换；三种写法都必须能正确解析
```

### 设备协议

| 链路 | 端口 | 帧格式 |
| --- | --- | --- |
| RFID 推送（WCS 主动连出） | TCP `2010` | ASCII 文本帧 `{SN0027\|01\|EPC}0D`，需处理**粘包/拆包** |
| → PLC 分拣指令 | TCP `8192`（默认；`plcListenPort` 可配，发布配置为 `2000`） | `{EPC\|格口\|小车}`，格口 3 位、小车 3 位，无帧尾 |
| ← PLC 落格反馈 | 同上 | 5 字段 `{EPC\|格口\|firstCar\|lastCar\|status}`，兼容旧 3 字段 |
| PLC 锁格状态（S7） | Snap7 | 读 `DB77`，偏移 0，**25 字节 = 200 位**位图，1 秒轮询，**上升沿 = 满箱锁格 → 触发 H7** |

---

## 🚀 快速开始

### 方式一：直接跑发布版（**推荐新手先做这个**）

`release_WcsHttpServer/` 里有编译好的可执行文件。

```text
1. 进入 release_WcsHttpServer/
2. 双击 WCS_httpServer.exe
3. 启动后会自动连接 PLC/RFID，并自动执行一次「开始接收任务」
```

> ⚠️ **必须先有对端**：直接双击会一直显示"重连中"，因为它在等 RFID 和 PLC。
> 请先按**方式二**启动仿真台，或先改配置指向你的实际设备。

**不用真设备也能全链路跑通** —— 见下面。

### 方式二：无设备全链路仿真（**最推荐的入门方式**）

`mock_env/` 是一个纯 Python（**标准库 + tkinter，零第三方依赖**）的仿真台，
在本机用软件扮演 **RFID 推送端 / PLC（含 S7 锁格）/ WMS 网关** 三方：

```text
┌────────────────────── 你的一台电脑 ──────────────────────┐
│                                                          │
│  WCS_httpServer.exe                                      │
│    ● 监听 8191  ← 仿真台推送 H4/H6/H5                     │
│    ● 监听 8192  ← 仿真 PLC 连入，收 {EPC|格口|小车} 并回执  │
│    ● 连出 2010  → 仿真 RFID 推送服务（发 EPC 帧）          │
│    ● 连出 S7    → 仿真 PLC S7（读 DB77 锁格位图）          │
│    ● 连出 9100  → 仿真 RFID 查询（EPC → SKU）             │
│    ● 连出 8099  → 仿真 WMS 网关（收 H7/H8 并应答）         │
│                                                          │
│  mock_app.py（仿真台 GUI，双击 run_mock.bat）             │
└──────────────────────────────────────────────────────────┘
```

```powershell
# 第 1 步：一键切到 mock 配置（会先自动备份正式配置）
mock_env\启用Mock配置.bat

# 第 2 步：启动仿真台（自动拉起 5 个仿真服务）
mock_env\run_mock.bat

# 第 3 步：启动 WCS
release_WcsHttpServer\WCS_httpServer.exe

# 第 4 步：在仿真台「场景与演练」页点「标准全流程」
#         按提示在 WCS 界面点两次按钮 —— 全链路就跑完了
```

仿真台会**逐条保留所有报文**（时间 / 通道 / 方向 / 原文 / HEX / 解析），
可以随时手工改包重发、制造超时、模拟空读帧。

📖 详细操作见 **[`mock_env/使用手册.md`](mock_env/使用手册.md)**

> 💡 **端口冲突提醒**：若本机 `plcListenPort` 与 S7 端口（102）冲突，
> 请先把 `plcListenPort` 改成如 `2000` 再启用 mock 配置。

### 方式三：从源码构建

**依赖清单**（版本需匹配，否则 moc/链接会失败）：

| 依赖 | 版本 | 用途 |
| --- | --- | --- |
| Windows + Visual Studio | 2019，平台工具集 **v142**，**x64** | 唯一构建目标 |
| Qt | **5.15.2 msvc2019_64** | Core / Gui / Widgets / Network / **Sql** |
| Qt VS Tools | `QtMsBuild`（工程内 `Keyword=QtVS_v304`） | moc / uic / rcc |
| HP-Socket | **5.8.5.4** | HTTP Server + PLC TCP Server + RFID TCP Client |
| Snap7 | 仓库内自带 | S7 通信（读 DB77 锁格） |
| SQLite | 3.46.0（`sqlite3.c` 已纳入编译） | 本地持久化（经 Qt `QSQLITE` 驱动访问） |
| log4cxx / hlog | 仓库 `include/` `lib/` | 分类日志 |
| QCustomPlot | 2.1.1（源码直接编译） | 效率统计图表 |

```powershell
# 1. 用 VS2019 打开解决方案
WCS_httpServer\WCS_httpServer.sln

# 2. 选择配置：Release | x64

# 3. 生成 —— 输出目录已配置为 ..\release_WcsHttpServer\
```

> ⚠️ **已知构建障碍（欢迎 PR 改善）**：
> `.vcxproj` 里的 include/lib 路径目前**硬编码**为 `D:\Qt\5.15.2\msvc2019_64` 和
> `D:\WCS\WCSApp\WCS_httpServer`，且 **Debug/Release 的宏与链接清单不一致**。
> 仓库尚无 CMake/qmake 工程与 CI 流水线。
> 移植时需修改 `WCS_httpServer.vcxproj` 中的 `IncludePath` / `LibraryPath`。

---

## 🖥 界面与操作闭环

现场作业是一个**固定闭环**，每一步都有明确的界面判据：

```text
开机自启 ──► 等待接收任务 ──► WMS 下发 H4 ──► 箱绑定（人工 / H6）
                                                      │
                                                      ▼
    ┌──────────── 点击「开始分拣」（灰→橙才可点）────────────┐
    │                                                      │
    ▼                                                      │
 分拣运行（面板计数 / 效率统计 / 运行日志）                    │
    │                                                      │
    ▼                                                      │
 一键满箱回传 H7 ──► WMS 确认 ──失败──► 重传满箱（下拉选失败格口）
    │                                                      │
    ▼                                                      │
 点击「结束任务」──► 补发 ──► 固定 2 秒 ──► H8 完结回传        │
    │                                                      │
    ▼                                                      │
 再次点击「开始接收任务」───────────────────────────────────┘
```

| 操作按钮 | 谁做 | 判据 / 边界 |
| --- | --- | --- |
| 开始接收任务 | 自动 + 人工 | 开机自动执行一次；点过「结束任务」后**不会自动接下一波** |
| 开始分拣 | **人工★** | 按钮灰→橙才可点（必须已绑定容器） |
| 一键满箱回传 | **人工★** | 二次确认；面板显示 `本波满箱回传 N 次 ｜ 本次成功 x / 失败 y` |
| 重传满箱 H7 | 人工 | 下拉选失败格口，或手输 `5` / `005` / `22005` |
| 结束任务 | **人工★** | 再点一次可立即停止；H8 未确认的报文下次自动补传 |
| 切换选中波次 | 人工 | 终态波次（已完成/已取消）按钮置灰，禁止切回 |

### 两条最常用的分支

```text
分支 A：中途换波 / 要接着做上一波
   做完 → 点「新任务」→ 进度/明细/绑定全部入库保留 → 等新波次
   没做完 → WMS 推新波次 → 历史列表选旧波次 → 点「切换选中波次」

分支 B：异常件与超计划
   落格异常（无匹配/无绑定/冲突/满箱/超计划）→ 物理异常口 66
        └─► 面板「处理 / 异常口」+1 + 写留痕
        └─► 到 66 口清出实物处置；若之后成功落格，计数自动 −1（闭环）
   超计划预警 → 点「查看」→ 看落了几件 / 哪个格口哪个容器 / 多余几件
```

📖 完整流程图（含可打印 SVG/HTML）：**[`docs/WCS现场作业流程图.md`](docs/WCS现场作业流程图.md)**
📖 标准作业程序 SOP：**[`docs/WCS软件标准作业程序SOP.md`](docs/WCS软件标准作业程序SOP.md)**

---

## 📂 项目结构

```text
WCS_httpServer/
├── WCS_httpServer/              ★ 当前有效工程（VS 工程 + 第一方源码）
│   ├── WCS_httpServer.vcxproj   MSBuild 工程（Qt VS Tools）
│   ├── main.cpp                 入口：崩溃捕获 + Qt 消息钩子 + 单实例互斥
│   ├── HttpServer.{h,cpp}       ★ 接入 + 波次编排 + 计划分配 + Outbox（最核心）
│   ├── MainWindow.{h,cpp}       ★ 全部运行界面与操作闭环
│   ├── PlcManager.{h,cpp}       PLC TCP 通信 + 选格决策 + S7 锁格轮询
│   ├── RfidPushClient.{h,cpp}   RFID TCP 客户端：重连/心跳/粘包拆包/原始报文留痕
│   ├── HttpClient.{h,cpp}       H7/H8 回传 + EPC→SKU 查询
│   ├── WaveManager.{h,cpp}      波次状态、计数、快照、超时重试
│   ├── ParseWorker.{h,cpp}      独立线程解析 H4，编译映射并留痕
│   ├── SortingDatabase.{h,cpp}  SQLite 门面：专用写线程 + 建表/迁移
│   ├── ConfigManager.{h,cpp}    XML 配置读写 + 热生效
│   ├── PlanAllocTable.h         ★ 计划分配表（额度守恒，无锁纯数据结构）
│   ├── PendingWaveQueuePolicy.h ★ 待执行队列「出队时机」纯判据
│   ├── WaveMapLogPolicy.h       ★ WAVE_MAP 留痕闸门纯判据
│   ├── EpcCode.h                ★ EPC 形态识别纯函数
│   ├── EpcCache.h               EPC→SKU/小车号 多源汇合缓存（TTL）
│   ├── DoubleBuffer.h           原子指针交换，整批发布 SKU 映射
│   ├── TaskQueue.h / ThreadPool.h  有界任务队列 / 通用线程池
│   ├── WmsGridCode.h            格口编码归一（22005 ↔ 005 ↔ 5）
│   ├── LifecycleLogger.h        EPC 生命周期事件记录
│   └── define.h                 网络/协议/容量/超时常量（含大量 SQL）
│
├── docs/                        ★ 现场文档库（50+ 份，本项目的知识精华）
│   ├── WCSHttpServer_软件架构分析报告.md    ← 先读这个！955 行深度审阅
│   ├── WCS现场作业流程图.md
│   ├── WCS软件标准作业程序SOP.md
│   ├── 计划分配表_设计与性能.md
│   ├── 波次归属与切回回溯_评估结论.md
│   ├── 意外关闭重启_继续上次任务.md
│   └── ...（20+ 份根因分析、现场验证用例、口径确认文档）
│
├── tests/                       16 个单测 + run_tests.bat（14 步回归，cl 直接编译产品类）
│   ├── test_plan_alloc_hard_ceiling.cpp   额度硬上限
│   ├── test_plan_alloc_blocked_grid.cpp   禁用格口
│   ├── test_pending_queue_gate.cpp        出队时机判据
│   ├── test_wave_map_policy.cpp           留痕闸门
│   ├── test_epc_recognize.cpp             EPC 形态识别
│   └── run_tests.bat
│
├── test/                        ★ 测试与仿真资产库（E2E 断言 / 模拟器 / 压测）
│   ├── e2e_rescan_resend.py     重扫重投 E2E（对真实 exe 断言）
│   ├── e2e_*.py                 满箱锁格 / 计划分配 / 波次隔离 / 未绑门禁 / 单元归属
│   ├── mock_wms.py / mock_plc.py / mock_rfid.py   三方对端模拟器
│   ├── 01_WMS模拟器.bat … 04_全流程端到端测试.bat   一键测试入口
│   ├── extreme_stress_test.ps1 / stress_test.py    压力测试
│   ├── analyze_crash_dump.ps1 + dumpsym.cs         崩溃转储符号解析
│   └── 仿真模拟操作手册.md
│
├── doc/                         上一波次恢复方案 / 生产上线评估报告
│
├── mock_env/                    RFID/WMS/PLC/S7 全链路仿真台（纯 Python 标准库）
│   ├── mock_app.py              仿真台 GUI（tkinter）
│   ├── run_mock.bat / 启用Mock配置.bat / 还原正式配置.bat
│   └── 使用手册.md
│
├── release_WcsHttpServer/      发布目录：exe + DLL + Qt 插件 + 运行配置 + 数据库
├── include/ lib/               第三方头文件与导入库（HP-Socket / log4cxx / Snap7 / QtXlsx / HControl）
├── tools/                      流程图渲染（build_flowcharts.js）+ SQLite 修复脚本
│
└── ※ 根目录另有 50+ 份项目分析文档（架构 / 协议 / 数据流 / 线程模型 / 测试方案 …）
```

### 官方文件之外的"野路子"

第一方约 **27 个模块、30,310 物理行**（不含供应商代码与 `src/` 旧版）。其中三个文件承担了大量跨层职责：

| 文件 | 行数 | 说明 |
| --- | ---: | --- |
| `HttpServer.cpp` | 8,828 | 接入协议 + 波次调度 + 恢复 + EPC 路由 + 计划分配 + 切出切回 + Outbox |
| `MainWindow.cpp` | 6,850 | 全部界面 + 应用编排 + 直接访问数据库 |
| `SortingDatabase.cpp` | 2,887 | 建表 / 迁移 / 全部读写 |

> 🎯 **给贡献者的机会**：这三个文件是重构的**首要目标**。
> 项目已经证明「纯逻辑头 + 单测锁口径」这条路可行（`PlanAllocTable` / `PendingWaveQueuePolicy` /
> `WaveMapLogPolicy` / `EpcCode` 四个纯逻辑头就是示范），把它们继续拆出去是最有价值的贡献方向。

---

## 🧪 测试与仿真

### 单元测试

```powershell
tests\run_tests.bat
```

用 `cl` 直接编译产品类进行验证，覆盖**业务不变量**而不只是函数返回值：

运行器共 **14 步**，全部驱动**真实产品类**（不是 mock）：

- 计划分配表硬上限（`已落 + 在途 ≤ 计划`）+ 分类/发货属性隔离
- 禁用格口掩码（未绑容器的格口绝不被认领，且额度不丢失）
- 待执行队列出队时机（只有点「开始接收」才允许出队）
- 留痕闸门（全量 / 首尾摘要 / 强制全量 / 退化）
- EPC 形态识别 + RFID 原始帧保留
- 关闭切出 / 重启新任务 / 切回恢复契约（含断电路径与终态禁切回）
- 锁格即解绑 + H7 报文保留 + H8 唯一出口
- 落格明细去重计数（面板「分拣件数」== 历史列表「已分拣」）
- outbox 回执留存（>1MB body 逐字符比对不截断）

### 离线仿真（不用设备、不用数据库，纯算法镜像）

```powershell
# 镜像现行的分配 / 落格 / 报文算法，验证多格口与人工失误场景
python docs\simulate_alloc.py
```

**这是理解分配算法最快的途径**：它不连设备、不写数据库，把"多格口分流 + 超计划改投 + H7 裁剪"
用 Python 重新实现了一遍，跑一遍就能看懂规则。

### 端到端验证（`test/` 测试资产库）

`test/` 是一整套可复用的测试与仿真资产：WMS/PLC/RFID 模拟器、E2E 断言脚本、压力测试。

```powershell
# 对真实 exe 跑重扫重投 E2E 断言
python test\e2e_rescan_resend.py

# 其他可直接跑的 E2E
python test\e2e_fullbox_lock_send.py        # 满箱锁格 → 解绑 → H7 发送
python test\e2e_plan_alloc_smoke.py         # 计划分配冒烟
python test\e2e_wave_isolation.py           # 跨波次隔离
python test\e2e_unbound_grid_gate.py        # 未绑容器不下发门禁
python test\e2e_unit_type_attribution.py    # 额度单元归属

# 压力测试 / 全流程
powershell -File test\extreme_stress_test.ps1
```

| 资产 | 用途 |
| --- | --- |
| `test\01_WMS模拟器.bat` / `02_PLC模拟器.bat` | 一键拉起单个模拟器 |
| `test\03_压力测试.bat` / `04_全流程端到端测试.bat` | 一键压测 / 全流程 |
| `test\mock_wms.py` `mock_plc.py` `mock_rfid.py` | 三方对端模拟器 |
| `test\analyze_crash_dump.ps1` + `dumpsym.cs` | 崩溃转储符号解析 |
| `test\仿真模拟操作手册.md` | 测试资产使用说明 |

### 仿真台自测

```powershell
# 校验仿真台自身的协议编解码与通道逻辑（纯 Python，无需 WCS）
python mock_env\selftest.py
```

> 📌 端到端联调也可直接用「方式二」的仿真台 + `mock_env\run_mock.bat`，
> 它会驱动**真实 exe** 跑完整链路并保留全部报文。

### 只读核对（生产环境安全）

```powershell
# 从 sorting_records.db 核对「计划 vs 实际落格」，只读打开，绝不写库
python docs\dump_wave_mapping.py
```

---

## 🎓 新手学习路线

**别从 `HttpServer.cpp` 第一行开始读**（8,828 行，会劝退）。按这个顺序：

| 阶段 | 看什么 | 你会得到 |
| :---: | --- | --- |
| **0** | **本 README** + `docs/WCS现场作业流程图.md` | 知道系统在干什么、有哪几条链路 |
| **1** | `mock_env/使用手册.md` → 跑一遍仿真台 | **看到** H4→H6→分拣→H7→H8 全流程真实报文 |
| **2** | `WCS_httpServer/main.cpp`（229 行） | 理解启动顺序、崩溃捕获、单实例保护 |
| **3** | 四个**纯逻辑头**（各 63~1163 行，无 IO、由调用方加锁）<br>`EpcCode.h`(70) → `PendingWaveQueuePolicy.h`(79) → `WaveMapLogPolicy.h`(63) → `PlanAllocTable.h`(1163) | 摸到项目的"业务规则内核"，**这是最好的切入点** |
| **4** | `tests/` 里对应的单测 | 看清每条不变量的边界条件（比读实现更快） |
| **5** | `docs/simulate_alloc.py` | 用 Python 看懂分配算法（离线镜像，7 场景） |
| **6** | `ConfigManager.h` + `config/http_server.xml` | 知道有哪些可调参数、默认值是什么 |
| **7** | `docs/WCSHttpServer_软件架构分析报告.md` | **读完整架构审阅**（含风险清单与优化路线） |
| **8** | `HttpServer.cpp`（按功能搜索，不要顺序读） | 深入核心编排 |
| **9** | `docs/` 里 20+ 份根因文档 | 学**怎么定位真实产线的疑难问题**——这部分最值钱 |

> 💡 **给初学者的三个建议**
> 1. **先跑起来再读代码**。仿真台能让你在半小时内看到完整业务流，比读三天代码有效。
> 2. **从纯逻辑头入手**。它们没有线程、没有 IO、没有锁，只有业务判断，最适合入门。
> 3. **读 `docs/` 里的根因文档**。那些"某个现场问题是怎么被查出来的"的记录，
>    是比代码本身更稀缺的学习材料。

### 技术栈（按需查阅）

```text
语言/标准      C++17（Release 显式 /std:c++17）、部分 C 代码（sqlite3.c）
框架           Qt 5.15.2（Widgets 桌面应用、QThread、QNetworkAccessManager、QtSql）
构建           MSBuild + Qt VS Tools（QtMsBuild），VS2019 x64
网络           HP-Socket 5.8.5.4（HTTP Server / TCP Server / TCP Client）
工业协议       Snap7（S7 通信，读写 PLC 数据块）
存储           SQLite 3.46.0（经 Qt QSQLITE 驱动），专用写线程 + 线程局部读连接
日志           log4cxx + hlog 封装，分类落文件（WCS/HTTP/Run/PLC/LIFECYCLE/DataBase）
图表           QCustomPlot 2.1.1
仿真/测试      Python 3（标准库 + tkinter）、批处理脚本
```

---

## ⚙️ 配置与部署

### 三套配置文件，别搞混

| 文件 | 角色 | 谁在用 |
| --- | --- | --- |
| `WCS_httpServer/config/http_server.xml` | **源码样例**（默认值参考） | 仅作参考，**不是运行配置** |
| `release_WcsHttpServer/config/http_server.xml` | **实际生效的配置** | 程序运行时读这个 |
| `mock_env/config_mock/http_server.xml` | **仿真模板** | 由「启用Mock配置.bat」写入上面那个 |

> ⚠️ **重要**：程序读取的是 **exe 所在目录**下的 `config/http_server.xml`——
> 不是源码目录里的那份。改错文件是新手最常见的坑。
> 源码样例往往落后于发布配置（例如 `plcListenPort` 样例为 `8192`、发布配置为 `2000`）。

### 易混淆的端口速查

```text
8191   WMS → WCS     HTTP 监听   wmsListenPort       （对外，WMS 推 H4/H5/H6）
8192   PLC → WCS     TCP  监听   plcListenPort       （默认；发布配置为 2000）
2010   WCS → RFID    TCP  连出   rfidPushServerPort  （WCS 主动连出）
102    WCS → PLC S7  Snap7 连出                      （读 DB77 锁格位图，1 秒轮询）
```

### 关键参数一览（默认值 / 现场值）

| 参数 | 默认 | 含义 |
| --- | :---: | --- |
| `businessPoolSize` | 90 | HTTP 业务线程池 |
| `plcSendPoolSize` / `plcRecvPoolSize` | 8 / 4 | PLC 发送池 / 反馈池 |
| `plcInFlightTimeoutMs` | 30000 | 下发后无落格反馈 → 解除在途（毫秒） |
| `PLC_SEND_TIMEOUT_MS` | 1000 | RFID 读到 → 下发 PLC 的实时性预算 |
| `rescanResendCooldownMs` | 1000 | 重复扫码抑制窗口 |
| `rescanResendMaxTimes` | 3 | 合法重投次数上限 |
| `rfidEpcTruncateLen` | 24 | EPC 形态识别长度（现场常配 25） |
| `exceptionGrid` | 66 | 超计划/异常件改投的物理异常口 |
| `sortingOverplanPolicy` | exception | 超计划处置：改投异常口 / block |
| `endReportDelayMs` | 2000 | 满箱补发完成 → H8 完结回传的固定间隔 |
| `allocEnabled` | true | 计划分配表开关（false = 回退旧逻辑） |
| `httpTimeoutMs` | 3000 | HTTP 回传超时（现场常配 10000） |
| `logRetainDays` | 30 | 日志保留天数 |

### 热生效 vs 需重启

```text
✅ 可热更新（UI 改完立即生效）
   回传 URL / AppKey / method、环境开关、HTTP 超时、RFID 查询与心跳、部分波次参数

❌ 必须重启
   监听端口、设备连接地址、线程池大小、面板开关（showLivePage / showPlanAllocPage / logEffPanelOn）
```

> 📌 热更新涉及**共享配置的并发读写**（架构报告 R10），修改时请避开分拣高峰。

### 部署形态

```text
release_WcsHttpServer/          ← 整个目录即部署单元，拷到现场机器即可
├── WCS_httpServer.exe          主程序
├── *.dll                       运行库（HP-Socket / log4cxx / Snap7 / sqlite3 …）
├── platforms/ sqldrivers/      Qt 插件（缺了会起不来）
├── config/http_server.xml      运行配置 ← 改这个
├── data/sorting_records.db     SQLite 数据库（自动创建）
└── log/                        分类日志
    ├── WCS/ HTTP/ Run/ PLC/ DataBase/ LIFECYCLE/
    ├── WAVE_MAP/wave_map.log   SKU→格口映射留痕（带体积闸门）
    └── crash_*.dmp crash.log   崩溃现场（配合 VS 打开 dmp 看调用栈）
```

---

## 🚨 生产就绪度与已知风险

这个项目**已经投入生产使用**，但它同样**诚实地记录了自己的技术债**。
这种透明度本身就是它的价值之一——比"一切都好"的 README 可信得多。

### ✅ 已经做对的

- 设备（PLC/RFID）生命周期与任务接收生命周期**分离**，一轮任务结束后无需重连设备
- 计划分配表：额度守恒 `remain = plan - landed - reserv`，三个唯一写点 + 30s 巡检
- 去重与合法重投分离，支持现场返工而不产生重复指令
- H7/H8 报文**先落库再发送**，支持自动重试 + 人工补传 + 切回补发
- 崩溃捕获（`minidump` + `crash.log` + 栈回溯 + Qt 最后消息）
- EPC 全生命周期日志、RFID 原始报文三层留痕、SKU→格口映射留痕（带体积闸门）
- 离线仿真 + 只读核对脚本，可在不碰生产数据的前提下验证算法

### ⚠️ 已知风险（摘自架构报告，按优先级）

| 编号 | 风险 | 影响 |
| :---: | --- | --- |
| **R3** | PLC 流完整性：跨回调拆包、反馈缓冲满时丢弃最旧 | 极端情况下丢落格反馈 |
| **R7** | Outbox 交付一致性：主路径写库失败仍会发送 | 断链时可能重复发送 |
| **R2** | 退出时的资源所有权与排空协议不完整 | 关闭瞬间的反馈可能未落库 |
| **R5** | 发送确认语义：缺少贯穿「下发→落格」的 commandId | 在途账与实绩账需人工核对 |
| **R10/R14/R17** | 配置并发与凭据脱敏 | 热更新共享配置存在竞态；日志/界面需脱敏 |
| **R1** | 全局映射先于激活交换，跨波次污染窗口未完全关闭 | 已部分修复，需带 waveId 的不可变快照 |
| **R19** | 产品源码反向 `#include` 了 `tests/BindingPanelPolicy.h` | 依赖方向倒置 |
| **R18** | exe/DLL/日志/数据库被整体提交进 git | 仓库体积偏大 |

📖 完整的 R1~R19 风险清单、源码证据行号与分阶段优化路线：
**[`docs/WCSHttpServer_软件架构分析报告.md`](docs/WCSHttpServer_软件架构分析报告.md)**（955 行，含第 5 节风险表与第 6 节优化建议）

> 🙌 **这些都是很好的 PR 机会。** 如果你正在学习工业软件架构，
> 这里有一份**真实项目、真实问题、真实证据**的清单等着你。

---

## ❓ 常见问题 FAQ

<details>
<summary><b>为什么是单进程而不是微服务？</b></summary>

现场部署成本优先：单进程 + SQLite 让现场只需拷贝一个目录就能运行，无需运维中间件。
代价是共享故障域、状态集中、扩展性受限。架构报告的建议是：**先在进程内把编排/设备适配/
持久化/UI 的职责分清**，再根据实际运维需求决定是否拆分——而不是为了"看起来先进"先拆。
</details>

<details>
<summary><b>为什么用 SQLite 而不是 MySQL/PostgreSQL？</b></summary>

单机部署、零运维、本地查询快、可直接用只读脚本核对数据（`mode=ro`）。
分拣场景的写入量（每件一条记录）远未触及 SQLite 瓶颈，真正的瓶颈在**线程归属**而不是数据库选型——
所以项目用"专用写线程 + 线程局部读连接"解决，而不是换数据库。
</details>

<details>
<summary><b>误关了窗口，正在分拣的波次会丢吗？</b></summary>

不会。点关闭 = **切出**当前波次：绑定归档 + 清内存，但进度/明细/未成功的 H7/H8 全部保留在数据库。
重新打开后在「波次数据历史记录」里选中该波次，点「切换选中波次」即可继续。
注意：**终态波次（已完成/已取消）不允许切回**，补报文请用重传按钮。
</details>

<details>
<summary><b>落格反馈超时了会怎样？</b></summary>

`plcInFlightTimeoutMs = 30000`：下发 30 秒无反馈则解除在途、释放额度，该件可重新发送。
同时 `alloc` 层的 `sweepExpiredClaims()` 会清理认领泄漏，`audit()` 每 30 秒巡检额度自洽性。
</details>

<details>
<summary><b>同一件货被 RFID 读到两次会发两条指令吗？</b></summary>

不会（默认配置下）。`rescanResendCooldownMs = 1000`——1 秒内的重复识别被视为噪声直接抑制。
超过冷却期的重复识别则按"合法重投"处理，受 `rescanResendMaxTimes = 3` 限制。
</details>

<details>
<summary><b>H7 失败了，还能发 H8 吗？</b></summary>

能，而且**必须能**。现场口径已定稿：点「结束任务」→ 把有数据的格口统一做一遍满箱回传 →
**固定延迟 `endReportDelayMs`（默认 2000ms）→ 不论 H7 是否成功都发送 H8**。
H7 与 H8 的结果完全分开，失败的 H7 报文保留可重试。
（早期设计里的"H8 完成屏障 + 人工决策弹窗"机制已确认不引入并已移除。）
</details>

<details>
<summary><b>能不用真实设备测试吗？</b></summary>

能，而且这是推荐的入门方式。`mock_env/` 用纯 Python 在单机上扮演 RFID/PLC/S7/WMS 四类对端，
一键切换配置（自动备份正式配置），所有报文可视化、可编辑、可重放。
详见 [`mock_env/使用手册.md`](mock_env/使用手册.md)。
</details>

<details>
<summary><b>为什么格口号有 22005 / 005 / 5 三种写法？</b></summary>

历史包袱 + 多系统约定：WMS 用「前缀 + 补零」编码（`22005`，前缀/宽度可配），
内部存储统一补零 3 位（`005`），操作员口述习惯说 `5`。
`WmsGridCode.h` 统一做归一，**协议边界归一、核心业务只处理明确类型的 ID** 是这个项目的设计原则之一。
</details>

---

## 📚 文档地图

**这个仓库的文档密度是它最大的隐藏财富。** 建议阅读顺序：

| 文档 | 行数级 | 适合谁 | 读它得到什么 |
| --- | :---: | --- | --- |
| [`README.md`](README.md) | 本文件 | 所有人 | 全局认知 |
| [`docs/WCS现场作业流程图.md`](docs/WCS现场作业流程图.md) | 149 | 现场/新人 | 开机到循环的完整作业闭环 |
| `docs/WCS现场操作手册.md` ⚠️ | — | 操作员 | 每个按钮什么时候点、判据是什么（**已随交接材料移出，见 `D:\WCSHttpServer-材料\doc\`**） |
| [`docs/WCS软件标准作业程序SOP.md`](docs/WCS软件标准作业程序SOP.md) | — | 现场管理 | 标准化作业流程 |
| [`mock_env/使用手册.md`](mock_env/使用手册.md) | 316 | **开发者★** | 无设备全链路联调 |
| [`docs/计划分配表_设计与性能.md`](docs/计划分配表_设计与性能.md) | — | **开发者★** | 核心算法的设计与取舍 |
| [`docs/WCSHttpServer_软件架构分析报告.md`](docs/WCSHttpServer_软件架构分析报告.md) | 955 | **架构/评审★** | 模块、线程、风险、优化路线全览 |
| [`docs/意外关闭重启_继续上次任务.md`](docs/意外关闭重启_继续上次任务.md) | — | 开发者 | 切出/切回与断电兜底设计 |
| [`docs/波次归属与切回回溯_评估结论.md`](docs/波次归属与切回回溯_评估结论.md) | — | 开发者 | 跨波次归属与回溯口径 |
| [`docs/MVP思想与需求分析.md`](docs/MVP思想与需求分析.md) | — | 产品/新人 | 需求是怎么一步步长出来的 |
| [`docs/`](docs/) 其余 20+ 份 | — | **所有人★** | 真实现场问题的根因分析与验证用例 |
| [`test/仿真模拟操作手册.md`](test/仿真模拟操作手册.md) | — | 测试/联调 | 测试资产库怎么用 |
| 根目录 50+ 份分析文档 | — | **所有人★** | 架构 / 协议帧 / 数据流 / 线程模型 / 数据库设计 / 测试方案 |
| 根目录 `提交信息.md` | 130KB | 考古 | 完整变更历史与背景（已随交接材料归档） |

**根目录分析文档速查**（挑最实用的）：

| 文档 | 看它解决什么 |
| --- | --- |
| `WCS_httpServer全盘业务分析报告.md` | 全局业务视角，比架构报告更偏业务 |
| `WCS_httpServer软件架构文档.md` | 模块与分层说明 |
| `WCS_httpServer接口说明文档.md` | 对外接口清单与字段 |
| `协议数据帧规范.md` | RFID/PLC 帧格式细则 |
| `数据流完整生命周期.md` / `业务数据流分析.md` | 一件货从下达到回传的全链路 |
| `线程模型详解.md` / `线程阻塞分析.md` | 并发与卡顿排查 |
| `数据库表设计方案.md` | 表结构与口径 |
| `现场联合调试指南.md` | 现场联调步骤 |
| `生产上线评估报告.md` / `部署上线修改清单.md` | 上线前检查 |
| `压力极限测试.md` / `完整测试方案.md` | 压测与测试策略 |
| `代码审查-高内聚低耦合.md` | 重构方向（对应架构报告第 6 节） |
| `PLC直连可行性分析及架构调整方案.md` | 设备接入方案对比 |

> 📌 `docs/` 目录里那些「XX问题_根因与修复_日期.md」是最有学习价值的材料：
> 它们记录了**一个真实故障是怎么被定位、验证、修复并回归的**——这种能力比会写代码更稀缺。

---

## 🤝 参与贡献

**非常欢迎贡献，尤其是下面这些"高价值、边界清晰"的方向：**

### 🌱 适合新手（Good First Issue）

- 📝 **补全文档**：给复杂函数加注释、翻译文档、补架构图
- 🧪 **补单测**：`tests/` 里的纯逻辑头最适合练手（无设备依赖、秒级编译）
- 🐍 **改善 mock_env**：仿真台是纯 Python，改起来快、收益直接
- 🔧 **给仿真台加新的异常场景**（丢包、迟到反馈、乱序……）

### 🚀 适合有经验者

- 🏗 **拆解 `HttpServer.cpp` / `MainWindow.cpp`**：按架构报告第 6 节的目标结构，把应用服务、设备适配、持久化、UI 分层
- 📦 **引入 CMake 构建 + CI**：干掉硬编码路径，让项目能在任意机器上构建（**当前最大痛点**）
- 🛡 **修复 R3（PLC 流完整性）/ R7（Outbox 一致性）/ R2（退出排空）**
- 🔒 **配置脱敏与版本化**：正式/mock 配置差异校验，日志中的凭据脱敏

### 提交规范

```text
提交信息使用中文，格式：[类型] 简明描述

类型标记（沿用现有习惯）：
  [add] 新增功能    [mod] 修改/优化    [bug] 缺陷修复    [doc] 文档
```

> 现有 52 个提交的 message 写得**非常详实**（说明问题背景、根因、影响范围），
> 这是很好的传统，请保持。

---

## ⚖️ 使用须知

### ⚠️ 安全免责声明

**本项目控制的是真实物理设备。** 未经充分验证的修改可能导致：

- 货物落错格口、设备异常动作
- 与 WMS 数据不一致，触发整条报文驳回
- 生产环境数据损坏或作业中断

**任何修改在上线前，务必**：① 在 `mock_env` 仿真环境完整验证；
② 用 `tests/run_tests.bat` 跑通单测；③ 在隔离环境做故障注入测试。

本软件按"现状"提供，作者不对生产环境中的任何损失承担责任。

### 🔐 敏感信息提醒

仓库中的 `WCS_httpServer/config/http_server.xml` 含**真实的接口地址与 AppKey 占位值**。
部署你的实例时，请**替换为自己的凭据**，并考虑将其移出版本控制。
（架构报告 R14/R17 已把"凭据脱敏"列为待整改项。）

### 📄 许可证

本仓库当前**尚未包含 LICENSE 文件**。若要正式开源，建议补充（如 MIT / Apache-2.0）。

> 在被授权方明确许可前，请将本仓库代码视为**保留所有权利**。

---

<div align="center">

### ⭐ 如果这个项目对你有帮助

它没有用任何炫技的框架，但它解决的是**真实的、会出货的、会出错的物理世界问题**。
如果它对你有启发，欢迎点个 **Star** —— 这也是对工业软件领域开源的一点支持。

**欢迎 Issue 交流，欢迎 PR 共建。**

`WMS` · `WCS` · `分拣系统` · `Qt` · `C++` · `工业自动化` · `PLC` · `Snap7` · `RFID` · `仓储物流`

</div>
