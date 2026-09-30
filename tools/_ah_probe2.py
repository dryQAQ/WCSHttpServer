# -*- coding: utf-8 -*-
"""1) 该件是否也出现在 H-T0460(格口48) 的 H7 里  2) 全站今天的 SKU 重复上报清单"""
import json
import re
import os
from collections import defaultdict

D = r"D:\共享文件夹\安徽奔赴自然-退货分拣设备"
RUN = os.path.join(D, "run.log")
SKU = "125214014601805"
DAY = "2026-09-30"
re_body = re.compile(r"body=(\{.*\})")

# SKU -> [(格口, 箱, qty, 时刻)]
by_sku = defaultdict(list)
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
        for it in obj.get("head", {}).get("detailList", []):
            by_sku[str(it.get("sku", ""))].append(
                (str(it.get("num", "")), str(it.get("targetLocation", "")),
                 int(it.get("qty", 0)), line[11:19]))

print("=" * 100)
print("① SKU %s 今天在 H7 里的全部出现" % SKU)
print("=" * 100)
for num, box, q, ts in by_sku.get(SKU, []):
    print("   %s  格口=%s  箱=%s  qty=%d%s" % (ts, num, box, q,
          "   ← 目标箱 H-T0460" if box == "H-T0460" else ""))

print()
print("=" * 100)
print("② 同一 SKU 被上报到 **多个格口/多只箱** 的清单（潜在错箱/串箱）")
print("=" * 100)
multi = {s: v for s, v in by_sku.items() if len({(n, b) for n, b, q, t in v}) > 1}
rows = []
for s, v in multi.items():
    tot = sum(q for _, _, q, _ in v)
    pairs = sorted({(n, b) for n, b, q, t in v})
    rows.append((len(pairs), tot, s, pairs))
rows.sort(key=lambda x: (-x[0], -x[1]))
print("共 %d 个 SKU 出现在 >1 个 (格口,箱) 组合里（合计上报件数 >1 即需核对）" % len(rows))
show = [r for r in rows if r[1] > 1]
print("其中『上报总件数 > 1』的 %d 个：" % len(show))
print("%-18s %6s  %s" % ("SKU", "总件数", "(格口,箱) 组合"))
for n, tot, s, pairs in show[:60]:
    print("%-18s %6d  %s" % (s, tot, " | ".join("%s/%s" % (a, b) for a, b in pairs)))
