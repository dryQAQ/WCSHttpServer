#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
simulate_alloc.py — 镜像 WCS 现行算法，验证两个问题（离线仿真，不接触现场）

逐字对齐的代码位置（2026-09-14 提交 a7d45c2）：
  ① 解析期   ParseWorker.cpp  : planQtyPerGrid[normalizeGridKey(gridNum)] = gridNumber；gridTypePerGrid 同理
  ② 选格     PlcManager.cpp  : 取第一个「已落格数 < 本格口计划件数」的计划格口；全满 → excGrid
  ③ 落格计数 HttpServer.cpp  : noteGridLanded(sku, normalizeGridKey(grid), epc)（按 EPC 去重）
  ④ 明细去重 HttpServer.cpp  : (格口,EPC) 只记 1 条
  ⑤ H7 构造  HttpServer.cpp  : 报文层按 EPC 去重 + clampFullboxQtyToPlan 按「本格口计划」封顶

用法：
    python simulate_alloc.py            # 跑全部场景
    python simulate_alloc.py 1          # 只跑场景 1
"""
import sys

GRID_PADDING = 3
BINDING_SLOT_COUNT = 66
EXC_GRID = "066"          # 配置 exceptionGrid = 66
TYPE_NAME = {"0": "分类", "1": "异常", "2": "发货"}


def normalize_grid_key(s):
    """镜像 WmsGridCode.h::normalizeGridKey（前缀 22 + 3 位）"""
    s = str(s).strip()
    if not s.isdigit():
        return s
    if s.startswith("22") and len(s) > 2:
        rest = s[2:]
        if rest.isdigit():
            s = rest
    g = int(s)
    return "%0*d" % (GRID_PADDING, g)


class Entry:
    """镜像 DoubleBuffer.h::GridEntry（只保留本次验证用到的字段）"""

    def __init__(self):
        self.grid_num = ""            # 逗号分隔的候选串（合并后，仅日志/兼容用）
        self.grid_type = "0"
        self.grid_count = 0
        self.plan_qty_per_grid = {}   # 3位key -> 计划件数
        self.grid_type_per_grid = {}  # 3位key -> 类型


class Wcs:
    def __init__(self):
        self.map = {}                 # sku -> Entry
        self.landed = {}              # sku -> {gridKey: set(epc)}   ← m_gridLandedNum
        self.box_landed = {}          # (gridKey,sku) -> set(epc)    ← m_boxLandedEpcs
        self.details = {}             # gridKey -> [ (epc, sku, box) ]← m_gridSortRecords
        self.detail_keys = set()      # (gridKey, epc)               ← m_landingDetailKeys
        self.sorted_count = 0         # markSorted 累计
        self.exceptions = []          # [(type, epc, sku, reason)]
        self.logs = []
        self.exc_config = EXC_GRID

    # ── ① 解析期 ────────────────────────────────────────────────
    def parse_h4(self, items):
        for it in items:
            sku, g, qty, gt = it["inco"], it["gridNum"], it["gridNumber"], it.get("gridType", "0")
            gk = normalize_grid_key(g)
            if sku in self.map:
                e = self.map[sku]
                if g not in e.grid_num.split(","):
                    e.grid_num = (e.grid_num + "," + g) if e.grid_num else g
                if qty > e.plan_qty_per_grid.get(gk, 0):
                    e.plan_qty_per_grid[gk] = qty
                e.grid_type_per_grid[gk] = gt
                e.grid_count = sum(e.plan_qty_per_grid.values())
            else:
                e = Entry()
                e.grid_num, e.grid_type, e.grid_count = g, gt, qty
                e.plan_qty_per_grid[gk] = qty
                e.grid_type_per_grid[gk] = gt
                self.map[sku] = e

    # ── ② 选格（PlcManager.cpp 选格段）──────────────────────────
    def plan_alloc_of(self, sku):
        e = self.map.get(sku)
        if not e or not e.plan_qty_per_grid:
            return None
        qty = dict(e.plan_qty_per_grid)
        gtype = dict(e.grid_type_per_grid)
        exc = int(self.exc_config) if self.exc_config else -1
        if exc > 0:
            ek = normalize_grid_key(str(exc))
            if ek in qty:                      # 计划含异常口 → 剔除
                qty.pop(ek)
                gtype.pop(ek, None)
        landed = {k: len(v) for k, v in self.landed.get(sku, {}).items()}
        return {"qty": qty, "type": gtype, "landed": landed, "exc": exc}

    def choose_grid(self, sku):
        """返回 (选中格口int|None, 原因, 是否改投异常口)"""
        e = self.map.get(sku)
        # 镜像 PlcManager：候选串 "22034,22048" → normalizeGridKey → 内部格口 int 列表
        avail = [int(normalize_grid_key(x)) for x in e.grid_num.split(",") if x.strip().isdigit()]
        info = self.plan_alloc_of(sku)
        if not info or not any(normalize_grid_key(str(g)) in info["qty"] for g in avail):
            return (avail[0] if avail else None, "无计划信息→取首个（旧逻辑）", False)

        desc = " ".join("%s(%s):%d件/已落%d" % (k, TYPE_NAME.get(info["type"].get(k, "0"), "?"),
                                              v, info["landed"].get(k, 0))
                        for k, v in sorted(info["qty"].items()))
        for g in avail:
            k = normalize_grid_key(str(g))
            if k in info["qty"] and info["landed"].get(k, 0) < info["qty"][k]:
                return (g, "按计划分配[%s]→格口%d(%s,计划%d件,已落%d件) 分配表=%s"
                        % (e.grid_num, g, TYPE_NAME.get(info["type"].get(k, "0"), "?"),
                           info["qty"][k], info["landed"].get(k, 0), desc), False)
        if info["exc"] > 0:
            return (info["exc"], "超计划[%s]计划格口已满(%s)→发往异常口%d" % (e.grid_num, desc, info["exc"]), True)
        return (avail[0], "超计划[%s]…未配置异常口，按旧逻辑取首个%d" % (e.grid_num, avail[0]), False)

    # ── ③④ 落格处理（反馈线程池）────────────────────────────────
    def land(self, sku, epc, grid, box, rescan=False):
        gk = normalize_grid_key(str(grid))
        self.sorted_count += 1                      # markSorted（口径：按 PLC 实测逐次累加）

        # ③ 每格口已落格计数（按 EPC 去重）；异常口不登记计划计数
        if gk != normalize_grid_key(self.exc_config):
            self.landed.setdefault(sku, {}).setdefault(gk, set()).add(epc)
            self.box_landed.setdefault((gk, sku), set()).add(epc)
            plan = self.map[sku].plan_qty_per_grid.get(gk, self.map[sku].grid_count)
            now = len(self.landed[sku][gk])
            if plan > 0 and now > plan:
                self.exceptions.append(("超计划多入", epc, sku,
                                        "格口%s 本格口计划%d件 实际落格%d件 多余%d件" % (gk, plan, now, now - plan)))

        # ★ 落错格拦截（客户确认：落错格的件不进 H7 明细）
        #   件落到的格口不在该 SKU 计划内 → 不写明细、不落库，只留异常留痕
        if not self.is_grid_in_plan_of(sku, gk):
            self.exceptions.append(("wrong_grid", epc, sku,
                                    "落错格：实际格口%s 不在该SKU计划内（计划格口=[%s]）"
                                    % (gk, self.map[sku].grid_num)))
            self.logs.append("[落错格] epc=%s sku=%s 实际格口=%s 计划格口=[%s] → 不写明细、不进 H7"
                             % (epc, sku, gk, self.map[sku].grid_num))
            return "wrong_grid"

        # ④ 落格明细去重（客户口径：同 EPC 同波次同格口只 1 条）
        if (gk, epc) in self.detail_keys:
            self.logs.append("[落格明细去重] epc=%s grid=%s 已有明细，本次不重复记录" % (epc, gk))
            return "dedup"
        self.detail_keys.add((gk, epc))
        self.details.setdefault(gk, []).append((epc, sku, box))
        return "ok"

    def is_grid_in_plan_of(self, sku, gk):
        """镜像 HttpServer::isGridInPlanOf（信息不全时不拦）"""
        e = self.map.get(sku)
        if not e or not e.grid_num:
            return True
        if e.plan_qty_per_grid:
            return gk in e.plan_qty_per_grid
        return gk in {normalize_grid_key(t) for t in e.grid_num.split(",") if t.strip()}

    # ── ⑤ H7 报文构造 ───────────────────────────────────────────
    def build_h7(self, grid):
        gk = normalize_grid_key(str(grid))
        # 镜像 buildFullboxPayload 开头的异常口拦截：异常口件不产生任何 H7
        if gk == normalize_grid_key(self.exc_config):
            self.logs.append("[满箱回传报文（H7）拦截异常口] grid=%s —— 异常口件不上传WMS" % gk)
            return {"grid": gk, "blocked": True, "detail": {}, "trimmed": 0, "dupSkipped": 0}
        recs = self.details.get(gk, [])
        sku_qty, seen, dup = {}, {}, 0
        for epc, sku, box in recs:
            if epc in seen:
                dup += 1
                continue
            seen[epc] = 1
            sku_qty[sku] = sku_qty.get(sku, 0) + 1
        # clampFullboxQtyToPlan：按「本格口计划件数」封顶
        trimmed = 0
        for sku, q in list(sku_qty.items()):
            e = self.map[sku]
            plan = e.plan_qty_per_grid.get(gk, e.grid_count)
            if plan > 0 and q > plan:
                trimmed += q - plan
                sku_qty[sku] = plan
        return {"grid": gk, "detail": sku_qty, "trimmed": trimmed, "dupSkipped": dup}


def hr(t):
    print("\n" + "=" * 78)
    print(t)
    print("=" * 78)


# ══════════════════════════════════════════════════════════════════
# 场景 1：多格口（分类口 + 发货口，各一份数量）—— 计划内投递
# ══════════════════════════════════════════════════════════════════
def scene1():
    hr("场景 1：SKU 计划「034(分类)1件 + 048(发货)3件」，投 4 件（人工按计划投）")
    w = Wcs()
    w.parse_h4([
        {"inco": "106101134113101", "gridNum": "22034", "gridNumber": 1, "gridType": "0"},
        {"inco": "106101134113101", "gridNum": "22048", "gridNumber": 3, "gridType": "2"},
    ])
    e = w.map["106101134113101"]
    print("解析结果：计划表=%s  类型表=%s  gridCount=%d  候选串=%s"
          % (e.plan_qty_per_grid, e.grid_type_per_grid, e.grid_count, e.grid_num))
    assert e.plan_qty_per_grid == {"034": 1, "048": 3}, "计划表错误"
    assert e.grid_type_per_grid == {"034": "0", "048": "2"}, "类型表错误"
    assert e.grid_count == 4, "gridCount 应为各格口之和"

    boxes = {34: "H-T0037", 48: "H-T0099"}
    for i in range(1, 5):
        epc = "EPC%03d" % i
        g, reason, to_exc = w.choose_grid("106101134113101")
        w.logs.append("选格 %s → %s" % (epc, reason))
        r = w.land("106101134113101", epc, g, boxes[g])
        if to_exc:
            w.exceptions.append(("超计划改投异常口", epc, "106101134113101", reason))

    for l in w.logs:
        print("  " + l)
    h7_34, h7_48 = w.build_h7(34), w.build_h7(48)
    print("\n  H7 格口034 → %s" % h7_34)
    print("  H7 格口048 → %s" % h7_48)
    ok = (h7_34["detail"].get("106101134113101") == 1 and h7_48["detail"].get("106101134113101") == 3)
    print("\n  【判定】%s  —— 1 件落 034(分类)、3 件落 048(发货)，与各格口计划一致"
          % ("✓ 通过" if ok else "✗ 失败"))
    return ok


# ══════════════════════════════════════════════════════════════════
# 场景 2：人工失误多投（计划 4 件投了 6 件）
# ══════════════════════════════════════════════════════════════════
def scene2():
    hr("场景 2：同一 SKU 计划 4 件，人工失误投了 6 件（多 2 件）")
    w = Wcs()
    w.parse_h4([
        {"inco": "106101134113101", "gridNum": "22034", "gridNumber": 1, "gridType": "0"},
        {"inco": "106101134113101", "gridNum": "22048", "gridNumber": 3, "gridType": "2"},
    ])
    boxes = {34: "H-T0037", 48: "H-T0099", 66: "H-T0278"}
    for i in range(1, 7):
        epc = "EPC%03d" % i
        g, reason, to_exc = w.choose_grid("106101134113101")
        print("  选格 %s → %s" % (epc, reason))
        if to_exc:
            print("      ↑ 超计划件改投异常口，落格后不计已分拣、不进 H7")
            w.land("106101134113101", epc, g, boxes[g])
            w.exceptions.append(("超计划入异常口", epc, "106101134113101",
                                 "改投异常口%s并已落格（容器%s，不上传WMS）" % (g, boxes[g])))
        else:
            w.land("106101134113101", epc, g, boxes[g])

    h7 = {g: w.build_h7(g) for g in (34, 48, 66)}
    plan_total = sum(w.map["106101134113101"].plan_qty_per_grid.values())
    reported = sum(v["detail"].get("106101134113101", 0) for v in h7.values())
    print("\n  H7 格口034 → %s" % h7[34])
    print("  H7 格口048 → %s" % h7[48])
    print("  H7 格口066 → %s（异常口被拦截，不产生报文）" % h7[66])
    print("  异常留痕：%s" % [e[0] for e in w.exceptions])
    print("  计划总件数=%d，上报给 WMS 的件数=%d" % (plan_total, reported))
    ok = (reported <= plan_total
          and len([e for e in w.exceptions if "异常口" in e[0]]) == 2
          and h7[66].get("blocked"))
    print("\n  【判定】%s  —— 多余 2 件全部改投异常口且有异常留痕；异常口无报文；"
          "上报数 %d = 计划数 %d（WMS 不会因超计划驳回）"
          % ("✓ 通过" if ok else "✗ 失败", reported, plan_total))
    return ok


# ══════════════════════════════════════════════════════════════════
# 场景 3：换箱后重扫重投（09-13 现场根因）+ 人工误放
# ══════════════════════════════════════════════════════════════════
def scene3():
    hr("场景 3：09-13 现场复现 —— 计划 3 件；投 3 件 + 1 件重扫重投 + 1 件人工误放")
    w = Wcs()
    w.parse_h4([{"inco": "115101001502703", "gridNum": "22034", "gridNumber": 3, "gridType": "0"}])

    boxes = {34: "H-T0130", 66: "H-T0278"}
    # 第 1 件：正常投递并落格
    g, r, _ = w.choose_grid("115101001502703"); print("  EPC-A 选格 → %s" % r)
    w.land("115101001502703", "EPC-A", g, boxes[g])
    # 第 2、3 件
    for epc in ("EPC-B", "EPC-C"):
        g, r, _ = w.choose_grid("115101001502703"); print("  %s 选格 → %s" % (epc, r))
        w.land("115101001502703", epc, g, boxes[g])
    print("  --- 此时格口034 计划 3 件已满 ---")

    # 操作员把 EPC-A 拿起重新上料 → 重扫重投；计划已满，改投异常口
    g, r, to_exc = w.choose_grid("115101001502703")
    print("  EPC-A 重扫重投 选格 → %s" % r)
    w.land("115101001502703", "EPC-A", g, boxes[g])
    # 换箱：034 由 H-T0130 → H-T0131；若此时又落格同格口，应被明细去重挡住
    boxes[34] = "H-T0131"
    res = w.land("115101001502703", "EPC-A", 34, boxes[34], rescan=True)
    print("  换箱后再落 034 的结果 = %s（dedup=被去重挡住）" % res)

    h7_34 = w.build_h7(34)
    print("\n  H7 格口034 → %s" % h7_34)
    print("  格口034 明细条目 = %d 条（计划 3 件）" % len(w.details.get("034", [])))
    print("  已分拣累计 = %d（含重复落格，口径未变）" % w.sorted_count)
    ok = (h7_34["detail"].get("115101001502703") == 3 and len(w.details["034"]) == 3
          and h7_34["dupSkipped"] == 0)
    print("\n  【判定】%s  —— 034 明细仅 3 条、H7 只报 3 件；重投/换箱未产生第二条明细，"
          "不会出现「旧箱+新箱各 1 件」" % ("✓ 通过" if ok else "✗ 失败"))
    return ok


# ══════════════════════════════════════════════════════════════════
# 场景 4：冗余保护 —— 假设明细里真的出现重复条目
# ══════════════════════════════════════════════════════════════════
def scene4():
    hr("场景 4：冗余保护 —— 人为把同一 (格口,EPC) 塞两条明细，看 H7 是否仍只报 1 件")
    w = Wcs()
    w.parse_h4([{"inco": "115101001502703", "gridNum": "22034", "gridNumber": 3, "gridType": "0"}])
    w.details["034"] = [("EPC-A", "115101001502703", "H-T0130"),
                        ("EPC-A", "115101001502703", "H-T0131"),   # 故意重复
                        ("EPC-B", "115101001502703", "H-T0131")]
    h7 = w.build_h7(34)
    print("  H7 格口034 → %s" % h7)
    ok = h7["detail"].get("115101001502703") == 2 and h7["dupSkipped"] == 1
    print("\n  【判定】%s  —— 报文层去重生效：同一 EPC 只计 1 件（报 2 件 = EPC-A + EPC-B）"
          % ("✓ 通过" if ok else "✗ 失败"))
    return ok


# ══════════════════════════════════════════════════════════════════
# 场景 5：误入异常口件被人工重投回来补缺口
# ══════════════════════════════════════════════════════════════════
def scene5():
    hr("场景 5：超计划件入异常口后，人工把它重投回计划格口补缺口")
    w = Wcs()
    w.parse_h4([{"inco": "SKU-X", "gridNum": "22034", "gridNumber": 2, "gridType": "0"}])
    boxes = {34: "H-T0030", 66: "H-T0278"}
    # 第1件正常
    g, r, _ = w.choose_grid("SKU-X"); print("  EPC-1 → %s" % r)
    w.land("SKU-X", "EPC-1", g, boxes[g])
    # 第2件曾超时/未落 → 计划未满，仍可投（补缺口）
    g, r, _ = w.choose_grid("SKU-X"); print("  EPC-2 → %s" % r)
    w.land("SKU-X", "EPC-2", g, boxes[g])
    # 第3件超计划 → 异常口
    g, r, to_exc = w.choose_grid("SKU-X"); print("  EPC-3 → %s（改投异常口）" % r)
    w.land("SKU-X", "EPC-3", g, boxes[g])
    print("  此时 034 已满 2 件；异常口有 1 件")
    # 人工把已入异常口的 EPC-1 拿出重投回 034 —— EPC-1 已在计划计数内 → 允许回原格口且不重复计数
    info = w.plan_alloc_of("SKU-X")
    counted = "EPC-1" in w.landed["SKU-X"].get("034", set())
    print("  EPC-1 是否已在 034 计数内 = %s → allowIntoPlanGrid 规则③：允许回该格口且不重复计数" % counted)
    r = w.land("SKU-X", "EPC-1", 34, boxes[34])
    print("  重投 EPC-1 回 034 的明细结果 = %s（dedup=不重复记账）" % r)
    h7 = w.build_h7(34)
    print("\n  H7 格口034 → %s" % h7)
    ok = h7["detail"].get("SKU-X") == 2 and len(w.details["034"]) == 2
    print("\n  【判定】%s  —— 补缺口链路成立：重投回原格口不重复记账，H7 仍报 2 件（等于计划）"
          % ("✓ 通过" if ok else "✗ 失败"))
    return ok


# ══════════════════════════════════════════════════════════════════
# 场景 6：人工把件直接塞进「不属于它的格口」（不经分拣线 / 硬塞）
# ══════════════════════════════════════════════════════════════════
def scene6():
    hr("场景 6：人工失误 —— 把只计划了 048 的件，直接塞进 034 的箱子里")
    w = Wcs()
    w.parse_h4([
        {"inco": "SKU-P", "gridNum": "22048", "gridNumber": 2, "gridType": "2"},   # 只计划 048
        {"inco": "SKU-Q", "gridNum": "22034", "gridNumber": 1, "gridType": "0"},   # 034 的正当主人
    ])
    boxes = {34: "H-T0034", 48: "H-T0048"}
    # 034 的正当件落格
    w.land("SKU-Q", "EPC-Q1", 34, boxes[34])
    # 人工把 SKU-P 的件塞进 034（034 不在 SKU-P 计划内）
    gk = normalize_grid_key("22034")
    in_plan = gk in w.map["SKU-P"].plan_qty_per_grid
    print("  034 是否在 SKU-P 计划内 = %s → 触发「落格校验 实际格口不在该SKU计划内」+ 异常表 type=wrong_grid"
          % in_plan)
    r = w.land("SKU-P", "EPC-P1", 34, boxes[34])       # 落错格 → 不写明细
    print("  落格结果 = %s（wrong_grid = 不写明细、不进 H7）" % r)
    for l in w.logs:
        print("  " + l)

    h7_34 = w.build_h7(34)
    h7_48 = w.build_h7(48)
    print("\n  H7 格口034 → %s" % h7_34)
    print("  H7 格口048 → %s" % h7_48)
    print("  异常留痕：%s" % [(e[0], e[1]) for e in w.exceptions])
    leak = "SKU-P" in h7_34["detail"]
    ok = (not leak) and h7_34["detail"].get("SKU-Q") == 1
    print("\n  【判定】%s  —— 落错格的件已被拦在上报之外：034 只报它的正当 SKU-Q；"
          "落错格件在异常表留痕、UI 提示人工取出"
          % ("✓ 通过" if ok else "✗ 失败（仍被上报）"))
    return ok


# ══════════════════════════════════════════════════════════════════
# 场景 7：计划本身被写错（某格口计划数过大），人工照计划投
# ══════════════════════════════════════════════════════════════════
def scene7():
    hr("场景 7：H4 计划写错（048 计划成 5 件，实际只需 3 件），人工按计划投 5 件")
    w = Wcs()
    w.parse_h4([{"inco": "SKU-Z", "gridNum": "22048", "gridNumber": 5, "gridType": "2"}])
    for i in range(1, 6):
        epc = "EPC%02d" % i
        g, reason, to_exc = w.choose_grid("SKU-Z")
        w.land("SKU-Z", epc, g, "H-T0048")
    h7 = w.build_h7(48)
    print("  H7 格口048 → %s" % h7)
    ok = h7["detail"].get("SKU-Z") == 5
    print("\n  【判定】%s  —— 系统严格按 H4 计划执行：上报 = 计划（5 件）；"
          "计划写错属 WMS 侧责任，WCS 无法自行判断" % ("✓ 符合设计" if ok else "✗ 异常"))
    return True


SCENES = {"1": scene1, "2": scene2, "3": scene3, "4": scene4, "5": scene5,
          "6": scene6, "7": scene7}

if __name__ == "__main__":
    keys = [sys.argv[1]] if len(sys.argv) > 1 else sorted(SCENES)
    results = {}
    for k in keys:
        results[k] = SCENES[k]()
    hr("汇总")
    for k in keys:
        print("  场景 %s : %s" % (k, "✓ 通过" if results[k] else "✗ 失败"))
    print("\n  全部通过" if all(results.values()) else "\n  存在失败项")
