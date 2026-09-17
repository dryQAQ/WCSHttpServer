import io, re, collections

PLC  = r"C:\Users\18352\Desktop\log\PLC.log"
HTTP = r"C:\Users\18352\Desktop\log\http.log"
T0, T1 = '2026-09-14 08:31', '2026-09-14 09:24'
TARGETS = {
    '106101134113101': '22034',
    '1134010560402':   '22017',
}

# ── 1) http.log：EPC → SKU 映射（"EPC绑定 已存入EpcCache" 行，本波次时段全量） ──
epc2sku = {}
with io.open(HTTP, 'r', encoding='utf-8', errors='replace') as f:
    for L in f:
        if '已存入EpcCache' not in L: continue
        ts = L[:19]
        if not (T0 <= ts <= T1): continue
        m = re.search(r'epc=([A-F0-9]+) sku=(\S+)', L)
        if m: epc2sku[m.group(1)] = m.group(2)
print(f"本波次时段 EPC→SKU 映射条数 = {len(epc2sku)}")

# ── 2) PLC.log：落格反馈 {EPC|格口|首车|尾车|status} ──
fb = collections.defaultdict(list)   # (grid) -> [(ts, epc, status)]
with io.open(PLC, 'r', encoding='utf-8', errors='replace') as f:
    for L in f:
        if 'PLC反馈(5字段)' not in L: continue
        ts = L[:19]
        if not (T0 <= ts <= T1): continue
        m = re.search(r'raw=\{([^}]*)\}', L)
        if not m: continue
        parts = m.group(1).split('|')
        if len(parts) < 5: continue
        epc, grid, st = parts[0], parts[1], parts[4]
        fb[grid].append((ts, epc, st))
print(f"本波次时段 PLC 落格反馈行数 = {sum(len(v) for v in fb.values())}")
print("  涉及格口数 =", len(fb))
print("  各格口反馈条数(前12):", dict(list(sorted(fb.items(), key=lambda kv: -len(kv[1])))[:12]))

# ── 3) 目标 SKU 在各格口的实际落格 EPC（去重） ──
print("\n" + "=" * 96)
print("目标 SKU 的 EPC 级落格情况（PLC.log 为准，sku 由 http.log 的 EPC绑定 映射）")
print("=" * 96)
for sku, planned_grid in TARGETS.items():
    eps = [e for e, s in epc2sku.items() if s == sku]
    print(f"\n【SKU {sku}】本波次 EPC 绑定数 = {len(eps)}")
    if not eps:
        print("   未见该 SKU 的 EPC 绑定记录（可能扫描时段外或未扫描）")
        continue
    # 这些 EPC 的落格反馈
    stat = collections.defaultdict(list)   # grid -> [(epc, status)]
    for grid, lst in fb.items():
        for ts, epc, st in lst:
            if epc in eps:
                stat[grid].append((epc, st))
    if not stat:
        print("   这些 EPC 无落格反馈记录")
        continue
    for grid in sorted(stat):
        rows = stat[grid]
        uniq = {e for e, _ in rows}
        ok   = {e for e, s in rows if s == '1'}
        print(f"   格口 {grid}: 反馈 {len(rows)} 条, 去重 EPC {len(uniq)} 个, "
              f"status=1 的去重 EPC {len(ok)} 个  → {sorted(uniq)}")

# ── 4) 反证：计划件数 vs EPC 实际件数 ──
print("\n" + "=" * 96)
print("结论对照")
print("=" * 96)
for sku, planned_grid in TARGETS.items():
    eps = [e for e, s in epc2sku.items() if s == sku]
    print(f"  SKU {sku}: 扫描到的实物 EPC = {len(eps)} 个")
