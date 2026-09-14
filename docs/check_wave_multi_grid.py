#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_wave_multi_grid.py — 只读核对某个波次的「同品多格口」计划与落格情况

用法：
    python check_wave_multi_grid.py                       # 核对最新波次
    python check_wave_multi_grid.py PP202600000587        # 指定波次

数据源（只读打开，不会写库）：
    release_WcsHttpServer/data/sorting_records.db
      · return_wave_item  —— H4 落库的波次明细（一行 = 一个 (SKU,格口) 与该格口计划件数）
      · sorting_records   —— 实际落格明细（sku / grid_num / boxcode / barcode=EPC / sort_time）
      · exception_record  —— 异常留痕
      · wave_raw          —— H4 原始报文（完整未截断，可核对 items 原始字段）
    注：sort_txn 表存在但在现网未被写入，核对实际落格请用 sorting_records。

回答两个问题：
    ① 该波次有哪些 SKU 是「同品多格口」、每个格口各计划几件、类型是分类还是发货
    ② 实际落格是否落在计划格口内（多格口分配是否生效）
"""
import os
import sqlite3
import sys

DB = r"D:/WCS/WCSApp/WCS_httpServer/release_WcsHttpServer/data/sorting_records.db"
TYPE_NAME = {"0": "分类", "1": "异常", "2": "发货"}


def q(cur, sql, args=()):
    try:
        cur.execute(sql, args)
        return cur.fetchall()
    except sqlite3.Error as e:
        print("  [SQL错误] %s  → %s" % (e, sql[:90]))
        return []


def gkey(g):
    """格口号归一：return_wave_item 存 '1'，sorting_records 存 '001' → 统一成 3 位内部 key"""
    s = str(g or "").strip()
    if s.isdigit():
        return "%03d" % int(s)
    return s


def main():
    order = sys.argv[1] if len(sys.argv) > 1 else None
    if not os.path.exists(DB):
        print("找不到数据库：%s" % DB)
        return 1

    con = sqlite3.connect("file:%s?mode=ro" % DB, uri=True)
    cur = con.cursor()

    if not order:
        rows = q(cur, "SELECT order_code FROM return_wave_item ORDER BY id DESC LIMIT 1")
        if not rows or not rows[0][0]:
            print("return_wave_item 中暂无数据")
            return 1
        order = rows[0][0]
    print("核对波次：%s" % order)

    # ── ① 该波次明细（SKU,格口,计划件数,类型）────────────────────────────
    items = q(cur, "SELECT inco, grid_num, plan_qty, grid_type FROM return_wave_item "
                   "WHERE order_code=?", (order,))
    print("\n明细行数 = %d" % len(items))
    if not items:
        print("该波次无明细（可能已归档/被清理）")
        con.close()
        return 1

    per_sku = {}
    for inco, g, qty, gt in items:
        per_sku.setdefault(inco, []).append((gkey(g), int(qty or 0), str(gt or "0")))

    multi = {k: v for k, v in per_sku.items() if len({x[0] for x in v}) > 1}
    print("SKU 总数 = %d，其中同品多格口 = %d" % (len(per_sku), len(multi)))

    plan_total = sum(int(x[1]) for v in per_sku.values() for x in v)
    print("计划件数合计 = %d" % plan_total)

    type_cnt = {}
    for k, v in multi.items():
        for g, qty, gt in v:
            type_cnt[TYPE_NAME.get(gt, gt)] = type_cnt.get(TYPE_NAME.get(gt, gt), 0) + 1
    if multi:
        print("多格口 SKU 涉及的格口类型分布：" +
              "  ".join("%s=%d个格口" % (k, v) for k, v in sorted(type_cnt.items())))

    print("\n── ① 同品多格口清单（前 25 条）──")
    for i, (sku, gl) in enumerate(sorted(multi.items())):
        if i >= 25:
            print("  …（其余 %d 个）" % (len(multi) - 25))
            break
        desc = " + ".join("%s(%s):%d件" % (g, TYPE_NAME.get(gt, gt), qty) for g, qty, gt in gl)
        print("  %s  合计%d件  [%s]" % (sku, sum(x[1] for x in gl), desc))

    # ── ② 实际落格 vs 计划格口（现网用 sorting_records，sort_txn 未写）────
    txn = q(cur, "SELECT barcode, sku, grid_num, boxcode FROM sorting_records WHERE order_code=?", (order,))
    print("\n── ② 实际落格核对（sorting_records 行数=%d）──" % len(txn))
    if not txn:
        print("  无落格流水（该表可能按波次归档或未写入）")
    else:
        landed = {}
        for epc, sku, g, box in txn:
            landed.setdefault(str(sku), {}).setdefault(gkey(g), set()).add(str(epc))

        wrong = 0
        per_grid_ok = 0
        for sku, gmap in landed.items():
            plan = per_sku.get(sku)
            if not plan:
                print("  [计划外SKU] %s 落格于 %s" % (sku, sorted(gmap.keys())))
                continue
            plan_grids = {g for g, _, _ in plan}
            plan_qty = {g: qty for g, qty, _ in plan}
            if len(plan_grids) > 1:
                print("  ▸ 多格口SKU %s 计划[%s]" % (
                    sku, " ".join("%s:%d件" % (g, plan_qty[g]) for g in sorted(plan_grids))))
            for g, epcs in gmap.items():
                if g in plan_grids:
                    over = len(epcs) - plan_qty.get(g, 0)
                    flag = "  ← 超出本格口计划 %d 件" % over if over > 0 else ""
                    if len(plan_grids) > 1:
                        print("     格口%s 实落%d件 / 计划%d件%s" % (g, len(epcs), plan_qty.get(g, 0), flag))
                    per_grid_ok += 1
                else:
                    print("     ✗ 落错格：%s 实际格口%s 不在计划%s（%d件）"
                          % (sku, g, sorted(plan_grids), len(epcs)))
                    wrong += 1
        print("\n  计划内落格格口组=%d  落错格格口组=%d" % (per_grid_ok, wrong))

    # ── ③ 异常留痕 ─────────────────────────────────────────────────────
    exc = q(cur, "SELECT type, COUNT(*) FROM exception_record WHERE order_code=? GROUP BY type", (order,))
    print("\n── ③ 异常留痕按类型 ──")
    if not exc:
        print("  无")
    for t, c in exc:
        print("  %-24s %d" % (t, c))

    con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
