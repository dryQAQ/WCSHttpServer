# -*- coding: utf-8 -*-
"""
mock_scenario.py — 场景/自动流程引擎
流程中的所有报文收发与人工检查点都会写入报文总览（系统/S7/…通道）。
ctx 为应用注入的“胶水对象”，接口见 _CTX_INTERFACE 说明（mock_app 实现）。
"""
import json
import threading
import time
import urllib.error
import urllib.request

import mock_proto as proto

H4 = "/api/DispatchSortingCommand/InsertWaveInfo"
H6 = "/api/DispatchSortingCommand/BindingLatticePort"
H5 = "/api/DispatchSortingCommand/InsertWaveIn"


class Abort(Exception):
    pass


def _seq_gen():
    n = 0
    while True:
        yield "SN%04d" % (n % 99 + 1)
        n += 1


class StdPlan:
    """标准全流程参数"""
    def __init__(self, grids=3, skus_per_grid=2, qty=2, order_code="PP2026MOCK001",
                 pace_s=0.35, box_prefix="H-01-2"):
        self.grids = grids
        self.skus_per_grid = skus_per_grid
        self.qty = qty
        self.order_code = order_code
        self.pace_s = pace_s
        self.box_prefix = box_prefix


def run_standard(ctx, plan: StdPlan):
    """标准全流程：H6绑定 → H4波次 → (人工开始分拣) → RFID逐件 → PLC反馈 →
    每格口满额后 S7锁格 → 等H7 → 人工结束任务 → 等H8"""
    rep = []
    def log(t, warn=False):
        ctx.log(t, warn)
    def post(path, obj):
        return ctx.http_post(path, obj)

    wave, epc_map, pieces = proto.build_count_wave(
        plan.order_code, grids=plan.grids, skus_per_grid=plan.skus_per_grid, qty=plan.qty)
    pieces_by_grid = {}
    for (epc, sku, g) in pieces:
        pieces_by_grid.setdefault(g, []).append((epc, sku))
    grid_list = [str(g) for g in range(1, plan.grids + 1)]

    log(f"══════ 标准全流程开始 order={plan.order_code} 格口数={plan.grids} "
        f"件数={len(pieces)} orderQty={wave['orderQty']} ══════")

    if not ctx.checkpoint("步骤0：请确认 WCS 已启动并加载 MOCK 配置，然后在 WCS 界面点击「开始接收任务」（任务接收变绿/显示“接收中”）。\n"
                          f"（WCS 界面与仿真台同机：WCS_httpServer.exe 需已运行；仿真台各服务建议处于运行状态）"):
        return "已中止"

    # ── 1. H6 容器绑定 ──
    log(f"── 步骤1: H6 容器绑定 {plan.grids} 个格口 ──")
    ok_bind = 0
    for g in grid_list:
        hole = int(g)
        body = {"boxcode": "%s%03d" % (plan.box_prefix, hole), "latticehole": g}
        code, text = post(H6, body)
        good = False
        try:
            r = json.loads(text)
            good = r.get("code") in ("200", 200) or "收到信息" in text
        except Exception:
            good = "收到信息" in text
        if good:
            ok_bind += 1
            log(f"绑定格口{g} → {body['boxcode']} 成功")
        else:
            log(f"⚠绑定格口{g} 失败 HTTP={code} resp={text[:160]}", True)
        ctx.sleep(0.12)
    if ok_bind < len(grid_list):
        if not ctx.checkpoint(f"H6 绑定完成 {ok_bind}/{len(grid_list)}（有失败项）。请先在 WCS 界面处理（如点「开始接收任务」），确认后再继续"):
            return "已中止"

    # ── 2. H4 波次下发 ──
    log(f"── 步骤2: H4 波次下发 order={plan.order_code} items={len(wave['items'])} qty={wave['orderQty']} ──")
    ctx.query_mapping(epc_map)
    code, text = post(H4, wave)
    try:
        r = json.loads(text)
        ok_wave = str(r.get("code")) == "200"
    except Exception:
        ok_wave = '"code":"200"' in text or '"code": "200"' in text
    if not ok_wave:
        log(f"⚠H4 下发被拒 HTTP={code} resp={text[:300]}", True)
        return ("H4 下发失败。常见原因：WCS 未点「开始接收任务」（返回 500 未开始接收任务）、"
                f"orderQty 与明细不一致(strict)、orderCode 重复等。报文见总览。\n{text[:400]}")
    log(f"H4 下发成功 → {text[:200]}")

    # ── 3. 等待人工开始分拣 ──
    log("── 步骤3: 等待人工「开始分拣」 ──")
    if not ctx.checkpoint("步骤3：H4 已下发。请在 WCS 界面点击「开始分拣」（按钮呈橙色可点）后返回本窗点“继续”。\n"
                          "（提示：若按钮灰色，说明波次仍 CREATED 未落库/未绑定完成，稍等1~2秒再试；H6 全部绑定成功会自动推进 BOUND）"):
        return "已中止"
    ctx.sleep(0.8)

    # ── 4. 逐件 RFID 推送 + PLC 反馈 ──
    # 4.0 前置检查：WCS 必须已连上 RFID 通道（否则一件都推不进去，锁格/H7 全是假象）
    if not getattr(ctx, "rfid_connected", lambda: True)():
        log("⚠WCS 未连接 RFID 推送通道（连接数=0）。分拣指令不会产生，请先处理连接问题", True)
        log("处理办法：重启 WCS_httpServer.exe（启动约3秒后自动连入 RFID 2010），"
            "或在状态栏确认“RFID推送服务端 ● 已连WCS:1”后再重新运行本场景", True)
        return "中止：WCS 未连上 RFID 通道（重启 WCS 后重试本场景）"
    ctx.set_auto_ack(True, "1")
    log(f"── 步骤4: RFID 逐件推送 {len(pieces)} 件（节奏 {int(plan.pace_s*1000)}ms，PLC 自动回执状态=1）──")
    seq_it = _seq_gen()
    sent = 0
    missing = []
    send_fail = 0
    grid_ok = {g: 0 for g in grid_list}      # 每格口成功推送数
    grid_plc = {g: 0 for g in grid_list}     # 每格口等到PLC指令数
    # 按格口分组推进，便于满箱锁格
    for g in grid_list:
        for (epc, sku) in pieces_by_grid[g]:
            seq = next(seq_it)
            mark_time = time.time()
            ok, note = ctx.rfid_send(epc, seq)
            sent += 1
            log(f"[{seq}] 推 EPC={epc} sku={sku} 格口={g}（{sent}/{len(pieces)}） 发送{'成功' if ok else '失败:' + note}")
            if not ok:
                send_fail += 1
                missing.append((epc, sku, g))
                if send_fail >= 3:
                    log(f"⚠连续/累计 {send_fail} 件发送失败 → 提前中止（避免后续锁格空等）。"
                        f"处理办法：确认 WCS 运行中并重启 WCS 使其重连 RFID 通道，然后重新运行本场景", True)
                    return (f"中止：{send_fail} 件发送失败（WCS 未连接 RFID 通道）。"
                            f"请重启 WCS_httpServer.exe 后重试。详情见报文总览")
                continue
            grid_ok[g] += 1
            seen = ctx.plc_epc_seen(epc, mark_time)
            if not seen:
                wait = ctx.wait_until(lambda e=epc, t=mark_time: ctx.plc_epc_seen(e, t), 6.0,
                                      f"等待 WCS→PLC 指令 {epc}")
                if not wait:
                    log(f"⚠EPC {epc} 未在 6s 内等到 PLC 落格指令（可能 SKU 查询超时/发送超时1s/状态非分拣中/格口禁用），"
                        f"请查看 WCS 界面异常列表与报文总览", True)
                    missing.append((epc, sku, g))
                    continue
                seen = True
            grid_plc[g] += 1
            ctx.sleep(plan.pace_s)
        # 该格口件数已全部推送完成 → 模拟满箱：S7 锁格
        if pieces_by_grid[g] and grid_ok[g] == 0:
            log(f"── 格口{g}: 无成功推送件（{grid_ok[g]}成功/{len(pieces_by_grid[g])}件），跳过锁格/等H7 ──", True)
            continue
        if g in pieces_by_grid and pieces_by_grid[g]:
            log(f"── 格口{g} 件数已满({len(pieces_by_grid[g])}件，成功推送{grid_ok[g]}，PLC回执{grid_plc[g]})，"
                f"S7 锁格(DB77 bit{g})，等待 H7 满箱回传 ──")
            before = ctx.gw_last().get("h7_count", 0)
            ctx.s7_set_grid(int(g), True)
            got = ctx.wait_until(lambda b=before: ctx.gw_last().get("h7_count", 0) > b, 8.0,
                                 f"等待 H7 到达仿真台网关(格口{g})")
            if got:
                log(f"✔ 格口{g} H7 满箱回传已到达网关（可在 WMS网关页查看报文；默认自动按样例应答 success）")
            else:
                log(f"⚠格口{g} 锁格后 8s 未收到 H7。先确认两点：① WCS 进程仍在运行且状态为分拣中；"
                    f"② 该格确实有分拣记录（WCS界面 已分拣数）。其余可能：EPC未查出SKU(H7中止)/无容器绑定。"
                    f"请查报文总览与 WCS 日志", True)
            ctx.sleep(1.2)     # 留出 WCS 轮询上升沿
            ctx.s7_set_grid(int(g), False)   # 模拟现场换箱（物理解锁）
            ctx.sleep(1.5)

    # ── 5. 汇总 + 人工结束任务 ──
    log(f"── 步骤5: 推送完成 成功{len(pieces)-len(missing)}/{len(pieces)}，等待人工「结束任务」(H8) ──")
    if not ctx.checkpoint("步骤5：全部件已推送。请在 WCS 界面点击「结束任务」触发 H8 完结回传\n"
                          "（若刚才有未等到 PLC 指令的件，WCS 会写异常，H8 仍会触发，属正常演练）"):
        return "已中止"
    before8 = ctx.gw_last().get("h8_count", 0)
    got8 = ctx.wait_until(lambda b=before8: ctx.gw_last().get("h8_count", 0) > b, 25.0,
                          "等待 H8 完结回传到达网关")
    if got8:
        log("✔ H8 完结回传已到达网关（已按样例自动应答 success；WCS 收到 success 后任务完结、清空绑定）")
    else:
        log("⚠25s 内未收到 H8。请确认已点「结束任务」且 WCS 波次处于分拣/完结流程；另注意 H8 会话兜底 30s", True)
    ctx.sleep(1.0)
    log("══════ 标准全流程结束 ══════\n后续建议：新一波次前请重新执行 H6 容器绑定（上一波完结已清空绑定），"
        "或用 WCS「新任务」按钮清空后重新下发。")
    return "完成（详情见报文总览与 WCS 界面）"


def run_drill(ctx, drill_name, plan: StdPlan = None, context_grids=None):
    """异常演练。返回结束语。"""
    def log(t, warn=False):
        ctx.log(t, warn)

    if drill_name == "NOREAD":
        log("── 演练NOREAD: 推送 {SN|01|NOREAD}0D ──")
        ctx.rfid_send("NOREAD", "SN0001", note="NOREAD演练")
        ctx.sleep(2.5)
        log("预期：WCS 只记日志不处理（不进分拣、不查SKU、不发PLC）。可查 WCS run.log 与本总览 RFID 通道。")
        return "NOREAD 演练完成"

    if drill_name == "unknown_epc":
        log("── 演练未映射EPC: 推送一个查询服务中不存在的 EPC ──")
        epc = "A19999999999999999999999"
        ctx.rfid_send(epc, "SN0002", note="未映射EPC演练")
        log("WCS 将向查询服务发起查询 → 应答0命中 → 3s后重试(共3次) → 写“SKU查询超时”异常表。约需 12~14s，请稍候…")
        ctx.sleep(14)
        log("预期：RFID查询通道可见 3 次请求均 0 命中；WCS 界面异常列表出现该 EPC。")
        return "未映射EPC 演练完成（观察结果）"

    if drill_name == "query_delay":
        log("── 演练查询延迟>1s: 模拟 SKU 查询耗时 1.3s（EPC首达→发PLC 1s 窗口被超时拒发）──")
        ctx.query_set_rule(delay_ms=1300)
        epc, sku, g = ctx.pick_mapped_piece()
        if not epc:
            ctx.query_set_rule(delay_ms=0)
            return "无可用映射件，跳过（请先跑标准流程）"
        mark = time.time()
        ctx.rfid_send(epc, "SN0003", note="查询延迟演练")
        time.sleep(3.0)
        seen = ctx.plc_epc_seen(epc, mark)
        ctx.query_set_rule(delay_ms=0)
        if seen:
            log("⚠竟然收到了 PLC 指令（可能恰在 1s 内完成），观察为主")
        else:
            log("预期达成：该件未被发 PLC 指令，WCS 写“发送超时”异常（EPC首次推送距发送>1s 拒发）。")
        return "查询延迟演练完成"

    if drill_name == "fb_status23":
        log("── 演练反馈状态码 2/3: 自动回执改状态2（无格口）推一件 ──")
        epc, sku, g = ctx.pick_mapped_piece()
        if not epc:
            return "无可用映射件，跳过（请先跑标准流程）"
        ctx.set_auto_ack(True, "2")
        ctx.rfid_send(epc, "SN0004", note="反馈状态2演练")
        seen = ctx.wait_until(lambda: ctx.plc_epc_seen(epc, time.time() - 30), 6.0, "等待PLC指令")
        time.sleep(2.0)
        ctx.set_auto_ack(True, "1")
        log("预期：状态码2/3 的反馈 WCS 判为异常处理（不计分拣成功、写异常），不会 markSorted。"
            "可对照：再推同一 EPC 会因已发/异常状态被处理。观察 WCS 界面即可。")
        return "状态码演练完成（已恢复正常回执状态=1）"

    if drill_name == "h7_fail":
        log("── 演练 H7 失败→Outbox 30s 重试（需要当前波次存在可锁格格口）──")
        g = context_grids[0] if context_grids else None
        if not g:
            return "无活动格口上下文，跳过（请先跑标准流程）"
        ctx.gw_set_rule(mode="auto", success=False)
        ctx.s7_set_grid(int(g), True)
        log("已锁格。WCS 将发 H7 → 网关应答 success=false → WCS 保留 Outbox，约 30s 后自动重试。")
        log("请在 40 秒观察窗口内把 网关页 应答切回「成功」：")
        t0 = time.time()
        retry_seen = 0
        before = ctx.gw_last().get("h7_count", 0)
        while time.time() - t0 < 45 and not ctx.aborted():
            cur = ctx.gw_last().get("h7_count", 0)
            if cur > before:
                retry_seen += 1
                log(f"观察到 H7 重试到达（第{retry_seen}次），此时若网关已切回成功则本次将成功")
                before = cur
            time.sleep(3)
        ctx.s7_set_grid(int(g), False)
        ctx.gw_set_rule(mode="auto", success=True)
        log("H7 失败演练结束（未成功的报文可在 WCS「波次数据记录」选中后点「重传满箱切换(H7)」补发观察）")
        return "H7 失败演练完成"

    return f"未知演练: {drill_name}"
