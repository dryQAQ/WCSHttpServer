import io, re, json, collections

LOG  = r"C:\Users\18352\Desktop\log\run.log"
WAVE = "PP202600000587"

# ── H4 计划 ──
h4 = None
with io.open(LOG, 'r', encoding='utf-8', errors='replace') as f:
    for L in f:
        if WAVE not in L or 'WAVE_ITEM' not in L or 'InsertWaveInfo' not in L or 'body=' not in L:
            continue
        b = L[L.index('body=') + 5:].strip()
        if h4 is None or len(b) > len(h4):
            h4 = b
dec = json.JSONDecoder()
o, _ = dec.raw_decode(h4.lstrip())
order_qty = int(o['orderQty'])
items = o['items']

plan = collections.defaultdict(int)          # (sku, grid) -> 计划
plan_sku = collections.defaultdict(int)      # sku -> 计划合计（跨格口）
plan_grid_type = {}
for it in items:
    sku = str(it['inco']).strip(); g = str(it['gridNum']).strip()
    n = int(it.get('gridNumber', 0) or 0)
    plan[(sku, g)] += n
    plan_sku[sku] += n
    plan_grid_type[(sku, g)] = it.get('gridType')

# ── H7 实报 ──
msgs = {}
with io.open(LOG, 'r', encoding='utf-8', errors='replace') as f:
    for L in f:
        if WAVE not in L or '[WMS出站发送]' not in L or 'detailList' not in L:
            continue
        try:
            mid = re.search(r'context=fullbox_([0-9a-f]+)', L).group(1)
            ts  = L[:19]
            b = L[L.index('body=') + 5:].strip(); b = b[:b.rindex('}') + 1]
            dl = json.loads(b)['head']['detailList']
            rows = [(str(x['sku']).strip(), str(x['num']).strip(),
                     str(x['targetLocation']).strip(), int(str(x['qty']))) for x in dl]
            msgs.setdefault(mid, (ts, rows))
        except Exception:
            pass

actual = collections.defaultdict(int)        # (sku, grid) -> 实报
actual_sku = collections.defaultdict(int)
grid_boxes = collections.defaultdict(set)
for mid, (ts, rows) in msgs.items():
    for sku, num, box, q in rows:
        actual[(sku, num)] += q
        actual_sku[sku] += q
        grid_boxes[num].add(box)

# ── 口径一：SKU 全局（跨格口）──
print("=" * 96)
print("口径一：按 SKU 汇总（跨格口合计）—— 判断「该 SKU 整体是否超计划」")
print("=" * 96)
over_sku = [(s, plan_sku[s], actual_sku[s]) for s in actual_sku if actual_sku[s] > plan_sku[s]]
print(f"  实报 SKU 数 = {len(actual_sku)}")
print(f"  ★ SKU 总量超过其计划数的：{len(over_sku)} 个"
      + ("" if over_sku else "  —— 即：没有任何 SKU 的分拣件数超过它自己的计划件数"))
for s, p, a in over_sku[:20]:
    print(f"      {s} 计划{p} 实报{a}")

# ── 口径二：SKU × 格口 ──
print("\n" + "=" * 96)
print("口径二：按 (SKU, 格口) —— 判断「件是否被分到了非计划格口」")
print("=" * 96)
over_pos  = []   # 该格口实报 > 该格口计划
under_pos = []   # 该格口实报 < 该格口计划（且该 SKU 该格口有上报或为0）
for (sku, g), a in sorted(actual.items()):
    p = plan.get((sku, g), 0)
    if a > p:
        over_pos.append((sku, g, p, a, a - p, plan_grid_type.get((sku, g), '-')))
    elif a < p:
        under_pos.append((sku, g, p, a, p - a))

print(f"  实报 (SKU,格口) 组合 = {len(actual)}")
print(f"  ★ 实报 > 计划 的组合：{len(over_pos)} 个，合计多出 {sum(x[4] for x in over_pos)} 件")
print(f"\n  {'SKU':<20}{'格口':<8}{'该格口计划':<10}{'实报':<7}{'多出':<6}{'格口类型'}")
print("  " + "-" * 84)
for sku, g, p, a, d, t in over_pos:
    print(f"  {sku:<20}{g:<8}{p:<10}{a:<7}{d:<6}{t}")

# 这些 SKU 在其他格口是否有"计划了却没报"的缺口
print("\n" + "=" * 96)
print("③ 逐 SKU 核对：多出的件 是否 = 该 SKU 在其它格口「计划了但没报到该格口」的缺口")
print("=" * 96)
print(f"  {'SKU':<20}{'格口':<8}{'计划':<7}{'实报':<7}{'差':<6}{'计划格口明细(计划/实报)'}")
print("  " + "-" * 92)
for sku, g, p, a, d, t in over_pos:
    gs = sorted({g2 for (s2, g2) in plan if s2 == sku} | {g2 for (s2, g2) in actual if s2 == sku})
    detail = "  ".join(f"{g2}:{plan.get((sku,g2),0)}/{actual.get((sku,g2),0)}" for g2 in gs)
    print(f"  {sku:<20}{g:<8}{p:<7}{a:<7}{d:<+6}{detail}")

# ── 总结 ──
sum_over = sum(x[4] for x in over_pos)
print("\n" + "=" * 96)
print("④ 结论")
print("=" * 96)
print(f"  ● 计划总件数 orderQty = {order_qty}")
print(f"  ● 已上报 {sum(actual.values())} 件（进行中快照，日志止于 09:23:11，无 H8）")
print(f"  ● 超出【全 SKU 计划总量】的件        = {sum(a - plan_sku[s] for s, p, a in over_sku)} 件")
print(f"  ● 超出【该 SKU 在该格口计划数】的件  = {sum_over} 件（{len(over_pos)} 个 组合）")
print(f"  → 即：没有 SKU 被多分件；但有 {sum_over} 件落到了与计划不同的格口（跨格口错位）")
