# -*- coding: utf-8 -*-
"""查 125214014601805 这件被记在哪只箱/哪个格口的 H7 明细里"""
import json
import re
import os
from collections import defaultdict

D = r"D:\共享文件夹\安徽奔赴自然-退货分拣设备"
RUN = os.path.join(D, "run.log")
SKU = "125214014601805"
EPC = "A10125010301052920803454"
DAY = "2026-09-30"

re_body = re.compile(r"body=(\{.*\})")

hits = []
allgrid = defaultdict(int)
with open(RUN, "r", encoding="utf-8", errors="replace") as f:
    for line in f:
        if not line.startswith(DAY) or "Fullbox" not in line or "detailList" not in line:
            continue
        m = re_body.search(line)
        if not m:
            continue
        try:
            obj = json.loads(m.group(1))
        except Exception:
            continue
        head = obj.get("head", {})
        dl = head.get("detailList", [])
        for it in dl:
            sku = str(it.get("sku", ""))
            num = str(it.get("num", ""))
            box = str(it.get("targetLocation", ""))
            allgrid[(num, box)] += int(it.get("qty", 0))
            if sku == SKU:
                hits.append((line[:23], num, box, it.get("qty")))

print("=" * 96)
print("SKU %s 在今天的 H7 明细中出现情况" % SKU)
print("=" * 96)
if hits:
    for ts, num, box, qty in hits:
        print("  %s  格口=%s  箱=%s  qty=%s" % (ts, num, box, qty))
else:
    print("  （没有出现在任何 H7 明细里）")

print()
print("=" * 96)
print("今天各 (格口,箱) 的 H7 上报件数汇总")
print("=" * 96)
for (num, box), q in sorted(allgrid.items(), key=lambda x: -x[1]):
    print("  格口=%-6s 箱=%-14s 合计 %d 件" % (num, box, q))
