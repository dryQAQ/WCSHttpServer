import io, re, json, collections

LOG  = r"C:\Users\18352\Desktop\log\run.log"
WAVE = "PP202600000587"
CHECK = ['1134010560402', '106101134113101', '12340106502703', '11340105110704']

# ── 1) H4 中这几个 SKU 的计划原文（items 里它们的 gridNumber）──
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

print("=" * 96)
print("① H4 原始计划中，这几个 SKU 的计划原文")
print("=" * 96)
for it in o['items']:
    if str(it.get('inco', '')).strip() in CHECK:
        print(f"   inco={it['inco']:<20} gridNum={it['gridNum']:<7} gridNumber={it['gridNumber']:<4} gridType={it.get('gridType')}")

# ── 2) 这几个 SKU 的 H7 上报行 + 该报文对应的响应 ──
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
            rows = [(str(x.get('sku', '')).strip(), str(x.get('num', '')).strip(),
                     str(x.get('targetLocation', '')).strip(), int(str(x.get('qty', '')))) for x in dl]
            msgs.setdefault(mid, [ts, rows])
        except Exception:
            pass

# 响应（该波次的 H7 响应按 msgId 配对）
resp = {}
with io.open(LOG, 'r', encoding='utf-8', errors='replace') as f:
    for L in f:
        if '[WMS出站响应]' not in L: continue
        m = re.search(r'context=(?:fullbox|resendFullbox)_([0-9a-f]+)', L)
        if not m: continue
        mid = m.group(1)
        ok  = re.search(r'success=(\d)', L)
        bd  = re.search(r'body=(\{.{0,150})', L)
        resp.setdefault(mid, []).append((L[:19], ok.group(1) if ok else '?', bd.group(1) if bd else ''))

print("\n" + "=" * 96)
print("② 含这些 SKU 的 H7 报文 → 发送时间 / 响应结果")
print("=" * 96)
for mid, (ts, rows) in sorted(msgs.items(), key=lambda kv: kv[1][0]):
    hit = [r for r in rows if r[0] in CHECK]
    if not hit: continue
    g = rows[0][1]; box = rows[0][2]
    r = resp.get(mid, [])
    outcome = "★无响应" if not r else ("success=1 ✔" if r[-1][1] == '1' else "success=0 ✘")
    print(f"  {ts[11:]}  格口={g} 容器={box:<9} 报文{len(rows)}行  → {outcome}")
    for sku, num, box2, q in hit:
        print(f"        {sku:<20} qty={q}")

# ── 3) 全波次 H7 响应统计 ──
print("\n" + "=" * 96)
print("③ 该波次 H7 响应总体统计")
print("=" * 96)
ok = bad = none = 0
for mid in msgs:
    r = resp.get(mid)
    if not r: none += 1
    elif r[-1][1] == '1': ok += 1
    else: bad += 1
print(f"  报文 {len(msgs)} 条：success=1 {ok} 条，success=0 {bad} 条，无响应 {none} 条")
