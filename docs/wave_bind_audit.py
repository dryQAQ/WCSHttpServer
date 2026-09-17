#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wave_bind_audit.py —— 格口绑定「波次归属」**只读**核查报告（2026-09-17）

背景（现场问题原文）
--------------------
"历史波次任务和该波次下的格口绑定状态以及显示并不会随切回而回溯过来，有时还会清空。"

根因（取证结论，见 docs/格口绑定波次归属_根因与修复_20260917.md）
------------------------------------------------------------------
1) H6（`POST …/BindingLatticePort?latticehole=…&boxcode=…`）**报文里没有波次号**，
   绑定行的 order_code 只能取"内存当前波次"，取不到就只能写空串（历史上还会因绑 NULL 整行丢失）；
2) 切出/新任务后 120s"切出窗口"内到达的 H6 会被记到**刚切出的上一个波次**
   → 新波次零绑定、旧波次被污染（2026-09-15 23:53 的 66 行 H6 实为 456456 的绑定，
     却挂在 6565656 名下；切回 456456 时"无任何绑定可恢复"）。

本脚本做什么 / 不做什么
-----------------------
* **只读**（`mode=ro`）：不修改任何数据、不建任何约束、不写任何文件；
* 逐波次列出"绑定行数 / 绑定时间跨度 / 已落格 / 状态"，并标注两类嫌疑；
* 对确认要清洗的历史数据，**打印**（不执行）人工修复 SQL，由现场/DBA 决定是否执行。

标注口径
--------
A. `切回将无法恢复绑定`：非终态(≠6/8) + 已落格>0 + 绑定行=0
   → 切回该波次时按波次取不到任何绑定，只能靠"物理当前绑定"兜底显示（且那不算它的记录）。
B. `疑似误归属`：某波次名下有一段"批量绑定"（行数≥阈值 且 时间跨度≤窗口），
   且紧随其后（≤ 后续判定窗口）有另一个波次的 H4（return_wave.created_at），而后者绑定行=0
   → 强烈提示这段绑定其实属于**后来的那个波次**。
C. 空归属行（order_code=''）：统计数量与时间跨度（旧数据常见；新版本会保留并在 H4 到达时补齐）。

用法
----
    python docs/wave_bind_audit.py                        # 默认读 release_WcsHttpServer/data/sorting_records.db
    python docs/wave_bind_audit.py --db <db路径>
    python docs/wave_bind_audit.py --all-waves             # 列出全部波次（默认只列有绑定或有嫌疑的）
    python docs/wave_bind_audit.py --burst-min-rows 20 --burst-window 180 --follow-window 300

退出码：0 = 正常（无论是否有嫌疑）；2 = 数据库打不开/参数错误
"""

import argparse
import os
import sqlite3
import sys

# Windows 控制台中文输出兜底（不影响重定向到文件）
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

DEFAULT_DB = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "release_WcsHttpServer", "data", "sorting_records.db")

WAVE_STATUS = {
    0: "空闲", 1: "已下发", 2: "已绑定", 3: "分拣中", 4: "满箱同步中",
    5: "取消处理中", 6: "已取消(终态)", 7: "完结中", 8: "已完成(终态)", 9: "异常挂起",
}


def open_ro(path):
    """只读打开（WAL 库在程序运行时也可安全只读查询）"""
    uri = "file:%s?mode=ro" % path.replace("\\", "/")
    con = sqlite3.connect(uri, uri=True)
    con.row_factory = sqlite3.Row
    return con


def secs_between(a, b):
    """两个 'yyyy-MM-dd HH:mm:ss[.zzz]' 文本时间差（秒）；解析失败返回 None"""
    from datetime import datetime
    for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S"):
        try:
            return (datetime.strptime(b, fmt) - datetime.strptime(a, fmt)).total_seconds()
        except Exception:
            continue
    return None


def main():
    ap = argparse.ArgumentParser(description="格口绑定波次归属只读核查报告")
    ap.add_argument("--db", default=DEFAULT_DB, help="sorting_records.db 路径")
    ap.add_argument("--all-waves", action="store_true", help="列出全部波次（默认只列有绑定或有嫌疑的）")
    ap.add_argument("--burst-min-rows", type=int, default=20, help="判为「批量绑定」的最少行数（默认 20）")
    ap.add_argument("--burst-window", type=int, default=180, help="批量绑定的最大时间跨度秒数（默认 180）")
    ap.add_argument("--follow-window", type=int, default=300,
                    help="批量绑定之后多久内出现的 H4 视为「紧随其后」（默认 300 秒）")
    args = ap.parse_args()

    if not os.path.exists(args.db):
        print("[FAIL] 数据库不存在: %s" % args.db)
        return 2
    try:
        con = open_ro(args.db)
    except Exception as e:
        print("[FAIL] 无法只读打开数据库: %s (%s)" % (args.db, e))
        return 2

    cur = con.cursor()

    # ── 波次清单（含状态/已落格）──
    waves = {}
    for r in cur.execute(
            "SELECT w.order_code, w.status, w.created_at, w.updated_at, "
            "  (SELECT COUNT(*) FROM sorting_records s "
            "    WHERE s.order_code = w.order_code AND s.barcode <> '') AS sorted_cnt "
            "FROM return_wave w"):
        waves[r["order_code"]] = dict(
            status=r["status"], created=r["created_at"], updated=r["updated_at"],
            sorted=r["sorted_cnt"] or 0, binds=0, bmin="", bmax="")

    # ── 每波次绑定统计 ──
    for r in cur.execute(
            "SELECT order_code, COUNT(*) AS n, MIN(bind_time) AS t0, MAX(bind_time) AS t1 "
            "FROM grid_box_bind WHERE order_code <> '' AND boxcode <> '' GROUP BY order_code"):
        w = waves.get(r["order_code"])
        if w is None:   # 库里有绑定行但波次头已不在（历史清理）→ 也列出来
            w = dict(status=-1, created="", updated="", sorted=0)
            waves[r["order_code"]] = w
        w["binds"] = r["n"]
        w["bmin"] = r["t0"] or ""
        w["bmax"] = r["t1"] or ""

    # ── 空归属行 ──
    blank = cur.execute(
        "SELECT COUNT(*) AS n, MIN(bind_time) AS t0, MAX(bind_time) AS t1, "
        "       SUM(CASE WHEN active=1 THEN 1 ELSE 0 END) AS act "
        "FROM grid_box_bind WHERE order_code = ''").fetchone()
    total_rows = cur.execute("SELECT COUNT(*) FROM grid_box_bind").fetchone()[0]
    active_rows = cur.execute("SELECT COUNT(*) FROM grid_box_bind WHERE active = 1").fetchone()[0]

    suspects_b = {}

    def note(code, kind, text):
        suspects_b.setdefault(code, []).append((kind, text))

    # ── 嫌疑 B：批量绑定之后紧随一个"零绑定"的新波次 ──
    for code, w in waves.items():
        if w["binds"] < args.burst_min_rows or not w["bmin"] or not w["bmax"]:
            continue
        span = secs_between(w["bmin"], w["bmax"])
        if span is None or span > args.burst_window:
            continue
        for ycode, y in waves.items():
            if ycode == code or not y["created"]:
                continue
            gap = secs_between(w["bmax"], y["created"])
            if gap is None or gap < 0 or gap > args.follow_window:
                continue
            if y["binds"] == 0:
                note(code, "B",
                     "本波次名下 %d 行绑定集中在 %s ~ %s（跨度 %.0f 秒），"
                     "紧接着 %s 下发了波次 %s（状态=%s）而该波次绑定行=0 "
                     "→ 这批绑定**很可能属于 %s**"
                     % (w["binds"], w["bmin"], w["bmax"], span, y["created"], ycode,
                        WAVE_STATUS.get(y["status"], y["status"]), ycode))
                note(ycode, "B",
                     "本波次绑定行=0，但其 H4 紧随 %s 的 %d 行绑定之后 → 那些绑定可能本属本波次"
                     % (code, w["binds"]))

    # ── 输出 ──
    print("=" * 100)
    print("格口绑定「波次归属」只读核查报告（不修改任何数据）")
    print("  数据库: %s" % args.db)
    print("  绑定总行数=%d（active=%d）｜空归属行=%d（其中 active=%d，时间 %s ~ %s）"
          % (total_rows, active_rows, blank["n"], blank["act"] or 0,
             blank["t0"] or "-", blank["t1"] or "-"))
    print("=" * 100)

    rows = []
    for code, w in waves.items():
        # A 类：非终态 + 一点绑定记录都没有 → 切回该波次恢复不出任何绑定
        #   （已落格>0 更严重：说明确实干过活，却一条绑定都没留下）
        kind_a = (w["status"] not in (6, 8) and w["status"] >= 0 and w["binds"] == 0)
        kind_b = code in suspects_b
        if args.all_waves or w["binds"] > 0 or kind_a or kind_b:
            rows.append((code, w, kind_a, kind_b))
    rows.sort(key=lambda x: (x[1]["created"] or ""), reverse=True)

    print("\n%-22s %-12s %6s %7s  %-23s %-23s  %s"
          % ("波次", "状态", "绑定行", "已落格", "绑定起", "绑定止", "标注"))
    print("-" * 130)
    na = nb = 0
    for code, w, kind_a, kind_b in rows:
        marks = []
        if kind_a:
            marks.append("A:切回无法恢复绑定")
            na += 1
        if kind_b:
            marks.append("B:疑似误归属")
            nb += 1
        print("%-22s %-12s %6d %7d  %-23s %-23s  %s"
              % (code, WAVE_STATUS.get(w["status"], "未知(%s)" % w["status"]),
                 w["binds"], w["sorted"], w["bmin"] or "-", w["bmax"] or "-",
                 " / ".join(marks) if marks else ""))

    print("\n" + "=" * 100)
    print("汇总：A 类（切回将无法恢复绑定）= %d 个波次；B 类（疑似误归属）= %d 个波次" % (na, nb))

    if nb:
        print("\nB 类嫌疑明细：")
        for code in sorted(suspects_b.keys()):
            for kind, text in suspects_b[code]:
                print("  [%s] %s: %s" % (kind, code, text))

    if na or nb:
        print("\n" + "-" * 100)
        print("人工处置建议（默认**不动数据**；要清洗请先让 WCS 开发确认，再手工执行）：")
        if na:
            print("  · A 类波次（绑定行=0）：历史数据无法事后还原（当时就没落库/归属落空）。")
            print("    处置：让 WMS 重发 H6（会以该波次号落库），或接受「仅按物理当前绑定显示」。")
        if nb:
            print("  · B 类波次（疑似误归属）：可按如下 SQL 人工改判（先备份 DB！执行前逐条核对）：")
            for code in sorted(suspects_b.keys()):
                w = waves[code]
                if w["binds"] >= args.burst_min_rows:
                    for ycode, y in waves.items():
                        if ycode != code and y["binds"] == 0 and y["created"]:
                            gap = secs_between(w["bmax"], y["created"])
                            if gap is not None and 0 <= gap <= args.follow_window:
                                print("      -- 示例：把 %s 名下这批绑定改判给 %s（务必先核对时间与箱号清单）"
                                      % (code, ycode))
                                print("      -- UPDATE grid_box_bind SET order_code = '%s' "
                                      "WHERE order_code = '%s' AND bind_time >= '%s';"
                                      % (ycode, code, w["bmin"]))
            print("    改判后建议用 UI「查看绑定」或下面的 SQL 逐格复核。")
        print("\n  只读复核 SQL（= 切回恢复口径，按波次号替换 'X'）：")
        print("    SELECT g1.grid_num, g1.boxcode, g1.active, g1.bind_time, g1.unbind_time")
        print("      FROM grid_box_bind g1")
        print("     WHERE g1.order_code = 'X' AND g1.boxcode <> ''")
        print("       AND g1.rowid = (SELECT MAX(g2.rowid) FROM grid_box_bind g2")
        print("                        WHERE g2.grid_num = g1.grid_num AND g2.order_code = 'X')")
        print("     ORDER BY CAST(g1.grid_num AS INTEGER);")

    print("\n说明：本报告只读、不改数据。新版本起：H6 → 物理绑定落库（归属空则空串保留），")
    print("      H4 到达时自动把「本会话内、当前生效、尚未归属」的绑定补记到该波次（含切出窗口误归属纠偏），")
    print("      并在「波次数据历史记录」列表用「格口绑定」列 + 「查看绑定」弹窗展示。")

    con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
