#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
analyze_multi_grid_wave.py — 用 wave_raw（完整 H4 报文）+ sorting_records 核对多格口分配

用法：
    python analyze_multi_grid_wave.py                  # 最新有明细的波次
    python analyze_multi_grid_wave.py PP202600000587
    python analyze_multi_grid_wave.py --list           # 列出可用波次

只读打开 sorting_records.db，不写任何数据。
"""
import json
import os
import sqlite3
import sys

DB = r"D:/WCS/WCSApp/WCS_httpServer/release_WcsHttpServer/data/sorting_records.db"
TYPE_NAME = {"0": "分类", "1": "异常", "2": "发货"}


def conn():
    return sqlite3.connect("file:%s?mode=ro" % DB, uri=True)


def list_waves():
    con = conn(); cur = con.cursor()
    cur.execute("SELECT order_code, length(raw_body), received_at FROM wave_raw ORDER BY received_at DESC LIMIT 20")
    for oc, ln, at in cur.fetchall():
        cur2 = con.cursor()
        cur2.execute("SELECT COUNT(*) FROM sorting_records WHERE order_code=?", (oc,))
        n = cur2.fetchone()[0]
        cur2.execute("SELECT COUNT(*) FROM return_wave_item WHERE order_code=?", (oc,))
        m = cur2.fetchone()[0]
        print("  %-22s 报文%-9d 收到=%s  落格记录=%-6d 计划明细=%d" % (oc, ln, at, n, m))
    con.close()


def main():
    args = [a for a in sys.argv[1:]]
    if "--list" in args:
        print("wave_raw 中最近的波次：")
        list_waves()
        return 0

    con = conn(); cur = con.cursor()
    if args:
        order = args[0]
    else:
        cur.execute("SELECT order_code FROM wave_raw WHERE order_code LIKE 'PP%' ORDER BY received_at DESC LIMIT 1")
        r = cur.fetchone()
        order = r[0] if r else None
    if not order:
        print("wave_raw 无可用波次")
        return 1

    cur.execute("SELECT raw_body, received_at FROM wave_raw WHERE order_code=? LIMIT 1", (order,))
    row = cur.fetchone()
    if not row:
        print("wave_raw 中找不到波次 %s（用 --list 查看可用）" % order)
        return 1
    raw, recv = row
    print("波次 %s   报文收到时间 %s   原始长度 %d" % (order, recv, len(raw)))

    # ── H4 items 解析 ─────────────────────────────────────────────────
    try:
        doc = json.loads(raw)
    except Exception as e:
        print("JSON 解析失败：%s" % e)
        print("报文前 300 字：%s" % raw[:300])
        return 1
    items = doc.get("items") or []
    if not items and isinstance(doc.get("data"), dict):
        items = doc["data"].get("items") or []
    print("H4 items 条数 = %d   orderQty = %s" % (len(items), doc.get("orderQty")))

    per_sku = {}
    for it in items:
        sku = str(it.get("inco") or "").strip()
        g = str(it.get("gridNum") or "").strip()
        try:
            qty = int(it.get("gridNumber") or 0)
        except (TypeError, ValueError):
            qty = 0
        gt = str(it.get("gridType") or "0").strip() or "0"
        if not sku or not g:
            continue
        per_sku.setdefault(sku, []).append((g, qty, gt))

    multi = {k: v for k, v in per_sku.items() if len({x[0] for x in v}) > 1}
    plan_total = sum(x[1] for v in per_sku.values() for x in v)
    print("SKU 数 = %d  计划件数合计 = %d" % (len(per_sku), plan_total))
    print("★ 同品多格口 SKU 数 = %d  涉及件数 = %d"
          % (len(multi), sum(x[1] for v in multi.values() for x in v)))

    tcnt = {}
    for v in multi.values():
        for g, qty, gt in v:
            tcnt[TYPE_NAME.get(gt, gt)] = tcnt.get(TYPE_NAME.get(gt, gt), 0) + 1
    if multi:
        print("★ 多格口涉及的格口类型：" + "  ".join("%s=%d个" % kv for kv in sorted(tcnt.items())))

    print("\n── 同品多格口清单（前 30）──")
    for i, (sku, gl) in enumerate(sorted(multi.items())):
        if i >= 30:
            print("  …（其余 %d 个）" % (len(multi) - 30)); break
        desc = " + ".join("%s(%s):%d件" % (g, TYPE_NAME.get(gt, gt), q) for g, q, gt in gl)
        print("  %s  合计%d件  [%s]" % (sku, sum(x[1] for x in gl), desc))

    # ── 实际落格（sorting_records）────────────────────────────────────
    cur.execute("SELECT sku, grid_num, boxcode, barcode, sort_time FROM sorting_records WHERE order_code=?", (order,))
    landed = cur.fetchall()
    print("\n── 实际落格（sorting_records 行数 = %d）──" % len(landed))
    if not landed:
        print("  该波次无落格记录")
    else:
        by = {}
        for sku, g, box, epc, t in landed:
            by.setdefault(str(sku), {}).setdefault(str(g), []).append((str(epc), str(box), str(t)))

        wrong = 0; ok = 0; over = 0
        for sku, gmap in sorted(by.items()):
            plan = per_sku.get(sku)
            if not plan:
                print("  [计划外SKU] %s 落格于 %s" % (sku, sorted(gmap.keys())))
                continue
            pgrid = {g: q for g, q, _ in plan}
            if len(pgrid) > 1:
                print("  ▸ 多格口SKU %s 计划[%s]" % (
                    sku, " ".join("%s:%d件" % (g, q) for g, q in sorted(pgrid.items()))))
            for g, rows in sorted(gmap.items()):
                epcs = {r[0] for r in rows}
                if g in pgrid:
                    d = len(epcs) - pgrid[g]
                    tail = "  ← 超出本格口计划%d件" % d if d > 0 else ""
                    if len(pgrid) > 1:
                        print("      格口%s(%s) 实落%d件/计划%d件 容器=%s%s" % (
                            g, TYPE_NAME.get(next((x[2] for x in plan if x[0] == g), "0"), "?"),
                            len(epcs), pgrid[g], ",".join(sorted({r[1] for r in rows})), tail))
                    ok += 1
                    if d > 0: over += 1
                else:
                    print("      ✗ 落错格：实际格口%s 不在计划[%s]（%d件）" % (g, sorted(pgrid.keys()), len(epcs)))
                    wrong += 1
        print("\n  计划内格口组=%d  落错格组=%d  超计划格口组=%d" % (ok, wrong, over))

    # ── 异常留痕 ─────────────────────────────────────────────────────
    cur.execute("SELECT type, COUNT(*) FROM exception_record WHERE order_code=? GROUP BY type ORDER BY 2 DESC", (order,))
    exc = cur.fetchall()
    print("\n── 异常留痕 ──")
    print("  无" if not exc else "\n".join("  %-26s %d" % (t, c) for t, c in exc))

    # ── H7 上报（outbox_fullbox）────────────────────────────────────
    cur.execute("SELECT grid, status, COUNT(*) FROM outbox_fullbox WHERE order_code=? GROUP BY grid, status", (order,))
    ob = cur.fetchall()
    print("\n── H7 满箱回传（outbox_fullbox grid×status）──")
    print("  无" if not ob else "\n".join("  格口%-6s %-10s %d" % (g, s, c) for g, s, c in ob))

    con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
