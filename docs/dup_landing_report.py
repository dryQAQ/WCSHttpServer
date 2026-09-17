#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dup_landing_report.py —— 历史重复落格明细 **只读** 报告（2026-09-15）

背景
----
落格明细 sorting_records 一行 = 一次 PLC 确认落格 = 1 件。
09-14 之前，重复反馈 / 换箱前后会把**同一物理件**写成两条明细（同一 波次+格口+EPC 多条），
导致 H7 按容器聚合上报时数量虚高 → WMS 整条驳回（[2107632] 转移库存…无法分配）。

新数据自 09-14 起已被拦截（内存去重 + 切回时按 DB 重建去重集合），
但**历史库**里仍可能留有重复行 —— 本脚本只报告、不修改任何数据、不建任何约束。

口径
----
重复组 = (order_code, grid_num, barcode) 计数 > 1（barcode = EPC）
另附判据：同一组的记录是否落在**不同容器**（换箱前后各一条的典型特征）。

用法
----
    python dup_landing_report.py                       # 默认读 release_WcsHttpServer/data/sorting_records.db
    python dup_landing_report.py --db <db路径>
    python dup_landing_report.py --limit 100           # 最多列 100 组（默认 50）
    python dup_landing_report.py --csv out.csv         # 同时导出明细到 CSV

退出码：0 = 正常（无论是否有重复）；2 = 数据库打不开/参数错误
"""

import argparse
import csv
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


def open_ro(path):
    """只读打开（WAL 库在程序运行时也可安全只读查询）"""
    uri = "file:%s?mode=ro" % path.replace("\\", "/")
    con = sqlite3.connect(uri, uri=True)
    con.row_factory = sqlite3.Row
    return con


def main():
    ap = argparse.ArgumentParser(description="历史重复落格明细只读报告")
    ap.add_argument("--db", default=DEFAULT_DB, help="sorting_records.db 路径")
    ap.add_argument("--limit", type=int, default=50, help="最多列出多少组（默认 50）")
    ap.add_argument("--csv", default="", help="可选：把重复明细导出到该 CSV 文件")
    args = ap.parse_args()

    if not os.path.exists(args.db):
        print("[错误] 数据库不存在: %s" % args.db)
        return 2

    try:
        con = open_ro(args.db)
    except Exception as e:
        print("[错误] 打开数据库失败（是否被占用？只读模式仍失败请检查路径）: %s" % e)
        return 2

    print("数据库（只读）: %s" % args.db)
    print("口径: (order_code, grid_num, barcode) 计数>1 = 同一波次同一格口同一件出现多条明细")
    print("-" * 100)

    grp_sql = (
        "SELECT order_code, grid_num, barcode, COUNT(*) AS cnt, "
        "       MIN(id) AS first_id, MAX(id) AS last_id, "
        "       MIN(sort_time) AS first_time, MAX(sort_time) AS last_time, "
        "       COUNT(DISTINCT boxcode) AS box_cnt, "
        "       GROUP_CONCAT(DISTINCT sku) AS skus "
        "FROM sorting_records WHERE barcode <> '' "
        "GROUP BY order_code, grid_num, barcode HAVING cnt > 1 "
        "ORDER BY cnt DESC, order_code"
    )
    groups = list(con.execute(grp_sql))
    total_rows = sum(int(g["cnt"]) for g in groups)
    affected_waves = len({g["order_code"] for g in groups})

    print("重复组数: %d 组，涉及 %d 条明细（占总行数比例如下表）、%d 个波次"
          % (len(groups), total_rows, affected_waves))

    total_all = list(con.execute("SELECT COUNT(*) FROM sorting_records WHERE barcode <> ''"))[0][0]
    if total_all:
        print("库内落格明细总数: %d（重复行占比 %.2f%%）" % (total_all, 100.0 * total_rows / total_all))
    print("-" * 100)

    if not groups:
        print("[OK] 无重复落格明细 —— 历史数据干净，无需任何处理。")
        con.close()
        return 0

    print("%-24s %-6s %-26s %-4s %-8s %s" % ("波次号", "格口", "EPC", "条数", "容器数", "首/末落格时间"))
    for g in groups[:max(0, args.limit)]:
        print("%-24s %-6s %-26s %-4d %-8d %s ~ %s%s"
              % (g["order_code"], g["grid_num"], g["barcode"], g["cnt"], g["box_cnt"],
                 g["first_time"], g["last_time"],
                 "   ← 跨容器(换箱前后各一条)" if g["box_cnt"] > 1 else ""))
    if len(groups) > args.limit:
        print("…（其余 %d 组未显示，用 --limit 调整）" % (len(groups) - args.limit))

    if args.csv:
        try:
            with open(args.csv, "w", newline="", encoding="utf-8-sig") as f:
                w = csv.writer(f)
                w.writerow(["order_code", "grid_num", "epc", "id", "sku", "boxcode", "car_num", "sort_time"])
                for g in groups:
                    for r in con.execute(
                            "SELECT id, sku, boxcode, car_num, sort_time FROM sorting_records "
                            "WHERE order_code = ? AND grid_num = ? AND barcode = ? ORDER BY id",
                            (g["order_code"], g["grid_num"], g["barcode"])):
                        w.writerow([g["order_code"], g["grid_num"], g["barcode"],
                                    r["id"], r["sku"], r["boxcode"], r["car_num"], r["sort_time"]])
            print("-" * 100)
            print("明细已导出: %s" % args.csv)
        except Exception as e:
            print("[警告] CSV 导出失败: %s" % e)

    print("-" * 100)
    print("说明：本脚本**只读**——不修改数据、不建约束。")
    print("      新数据已由 09-14 内存去重 + 切回时按 DB 重建去重集合拦住，不会再新增重复。")
    print("      如需清洗历史重复行或加唯一约束，请先由业务确认口径（保留最早一条等）后另行处理。")
    con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
