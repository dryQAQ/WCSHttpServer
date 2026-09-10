# WMS 报文 Mock 测试台（wms_mock_gui）

鞋服窄带分拣项目 **WCS ↔ WMS 接口**联调/演练工具。
**客户机无需安装 Python**：随包提供单文件 `dist\wms_mock_gui.exe`，双击即用；源码版亦为纯 Python 标准库。

- 输入 **SKU 码 + 格口号**等业务参数 → 自动生成与生产一致的报文 → 报文可修改 → 发送到**可配置的接口地址**；
- 内置**模拟 WMS 网关**：报文**正确 → 应答成功样例**，**错误 → 应答 `success=false` + 具体原因**（生产网关同字段，WCS 只认 `success==true`）；
- 判定规则（结构 / method / AppKey / 波次计划 / 格口号 / 容器绑定 / SKU计划 / 数量上限 / SKU主档）与主数据均可界面维护，自动保存。

---

## 一、文件与启动

| 文件 | 说明 |
|---|---|
| `dist\wms_mock_gui.exe` | ★ 单文件免Python版（PyInstaller 打包，已含界面文件），复制到客户机双击即用 |
| `wms_mock_gui.py` | 源码主程序（开发/自检用，需本机有 Python） |
| `webui.html` | 界面文件（可放 EXE 同目录覆盖自定义；不提供时用内置版本） |
| `run.bat` | 一键启动：有 EXE 跑 EXE，否则跑 Python |
| `wms_mock_state.json` | 运行后自动生成：参数/规则/绑定表/波次计划库（删除=恢复出厂默认） |
| `wms_mock_gui.log` | 仅 EXE 模式生成：运行日志（排查用） |

启动（任选其一）：
- 双击 `dist\wms_mock_gui.exe`（无黑窗口，自动打开浏览器，默认 GUI http://127.0.0.1:8765、网关 0.0.0.0:8099）；
- 双击 `run.bat`；
- 源码版命令行：`python wms_mock_gui.py --gui-port 9000 --gw-port 8099 --no-browser`

**退出**：浏览器页面右上角「✕ 退出服务」（EXE 无控制台，不要直接关页面）；
重复双击 EXE 不会双开——会自动检测已有实例并只打开浏览器。
> 首次运行如被 SmartScreen/杀软提示，选「仍要运行」（PyInstaller 单文件程序常见误报，代码同源码版）。

---

## 二、四种报文与成功判定口径（与生产一致）

| 类型 | 方向 | 接口 / method | 报文要点 | 成功判定 |
|---|---|---|---|---|
| **H7** 满箱回传 | WCS→WMS | `…/product/skuClassificationTask/gwisSubProductClassifyOrder`，method 同名 | `head{orderCode,orderType:"01",warehouseCode,goodsOwner,fromLocation,createDate,…}` + `detailList[{num:格口编码,targetLocation:容器号,sku,qty}]` | 响应 JSON 顶层 `success==true` |
| **H8** 波次完结回传 | WCS→WMS | `…/productClasTask/gwisSubProductClassifyEndOrder`，method 同名 | `head{orderCode,orderType:"01",sumLocation,operuserDate,operuserCode,operuserName}` | 响应 JSON 顶层 `success==true` |
| **H4** 波次下发 | WMS→WCS | `POST /api/DispatchSortingCommand/InsertWaveInfo` | `{orderCode,orderQty,sobi,items:[{inco=SKU,gridNum,gridNumber,gridType}]}`；orderQty 须 = ΣgridNumber | HTTP200 且 `code=="200"` |
| **H6** 容器绑定 | WMS→WCS | `POST /api/DispatchSortingCommand/BindingLatticePort?latticehole=格口&boxcode=容器` | 查询参数报文（无 JSON 体） | HTTP200 且 `code=="200"` |

- 发送 H7/H8 时自动按 WCS 口径附加 `?appkey=…&method=…` 并在 Header 带 `AppKey`（可取消勾选）。
- 格口号对外编码 = 前缀 + 补零（默认前缀 `22` 宽度 3：格口 5 → `22005`）；解析兼容 `22005 / 005 / 5`。可在「③ 规则与主数据」修改。
- 地址预设：本机模拟网关(8099) / WMS测试环境 `wmstest.pelliot.com.cn:9090` / WMS正式环境 `wms.pelliot.com.cn` / 本机WCS(8191)，全部可编辑、可新增。

---

## 三、模拟 WMS 网关（页面第②页签）

监听 `0.0.0.0:<端口>`（默认 8099），**任意路径**的 POST 都按 H7/H8 处理——真实 WCS 的
`feedbackTestUrl / feedbackEndTestUrl` 无需改路径，只需把主机指向运行本程序的机器。

应答策略：
- **自动（默认）**：按规则引擎逐项校验（见下），全部通过 → 成功样例
  `{"success":true,"body":"产品分类框号库位完成分类--同步成功!","ts":"…"}`（H7；H8 为“产品分类任务完结--同步成功!”）；
  任一规则失败 → `{"success":false,"body":"同步失败：<原因>","ts":"…"}`。
- **强制成功 / 强制失败**：演练 WCS Outbox 自动重试（H7 约 30s×2、H8 约 5s×3）等场景。

接收记录实时展示：时间 / 类型 / method参数 / URL与Header的AppKey / 逐项校验明细 / 请求原文 / 应答。

### 校验规则（可开关）

| 规则 | 内容 |
|---|---|
| 报文结构与必填字段 | head/orderCode/detailList(或sumLocation) 缺失、sku 空、qty 非正整数等 |
| method 校验 | URL 参数 method 必须为 H7/H8 期望值 |
| AppKey 鉴权 | URL 参数与 Header 的 AppKey 必须存在且等于「期望AppKey」 |
| 波次计划校验 | orderCode 必须存在于波次计划库（H4 发送成功自动写入 / 手工导入） |
| 格口号校验 | 1~66 且属于该波次明细 |
| 容器绑定校验 | 格口已绑定容器且 `targetLocation` = 绑定容器号 |
| SKU 计划校验 | 明细 SKU 必须在该格口的波次计划中 |
| 数量上限校验 | qty ≤ 该格口该 SKU 的计划数量（模拟生产拒超收） |
| SKU 主档校验 | SKU 必须存在（默认关闭） |

主数据（页面第③页签）：期望AppKey（默认测试 `dz_bxh_dmwcs_cs`，正式 `dz_bxh_wcs_zs`）、method、仓库/货主、
格口编码、绑定表、波次计划库（可粘贴 H4 JSON 导入）、SKU 主档（自动汇总）、发送地址预设。
所有修改点「保存以上全部主数据」后自动写入 `wms_mock_state.json`。

---

## 四、把真实 WCS 的 H7/H8 接入本模拟网关

1. 本机联调：运行仓库 `mock_env/启用Mock配置.bat`，WCS 回传 URL 即指向 `127.0.0.1:8099`（本程序默认端口）；
   在 WCS 界面锁格（满箱）→ 这里收到 H7；点「结束任务」→ 收到 H8。
2. WCS 在其它机器：手工把 WCS `config/http_server.xml` 的 `feedbackTestUrl / feedbackEndTestUrl`
   主机改为**运行本程序的机器 IP**（网关默认监听 0.0.0.0），重启 WCS。
3. 用「强制失败」可观察 WCS 重试；恢复「自动」后下一发即按规则判定。
4. 想直连真实 WMS：H7/H8 预设选 wmstest/正式网关并核对 AppKey 后发送，页面显示真实网关响应。

> 演练闭环（全在本程序内）：H4 下发（自动记录计划）→ H6 绑定（自动记录绑定表）→
> H7 填计划内 SKU 得 success=true；故意填错 SKU/超量/错AppKey/未知波次/未绑定格口，得 success=false+原因。

---

## 五、常见问题

- **8099 被占用**：仿真台 mock_app.py 正在运行（两者共用该端口）；关闭其一，或在本程序改网关端口并同步 WCS 回传URL。
- **客户机没 Python**：用 `dist\wms_mock_gui.exe`，无需安装任何东西；文件夹里只放 EXE 一个文件即可运行。
- **发到 WCS 被 HTTP500“未开始接收任务”**：真实 WCS 防护（需在 WCS 界面点「开始接收任务」），按失败展示属正常。
- **H4 严格校验**：orderQty ≠ ΣgridNumber 会被真实 WCS 拒绝（结构预检会提示 ⚠）。
- **状态存哪**：EXE 同目录 `wms_mock_state.json`（日志不落盘、重启清空）；删除即出厂默认。
- **如何重新打包 EXE**（需本机 Python + 网络）：`python -m pip install pyinstaller`，
  在源码目录执行：`python -m PyInstaller --noconfirm --onefile --noconsole --name wms_mock_gui --add-data "webui.html;." wms_mock_gui.py`，
  产物在 `dist\wms_mock_gui.exe`。
- **已通过自检**：H7/H8/H4/H6 共 33 项端到端断言全部通过（正确→成功样例、各类错误→对应失败原因、格口号三种写法、
  规则开关生效、H4/H6 成功记录底账、失败不记录、代理原样透传、EXE 启动/发送/退出）。

---
*配套：仓库级联调仿真台见 `mock_env/使用手册.md`（全链路 RFID/PLC/S7），本工具专注 WMS 接口两侧报文演练，可独立使用。*
