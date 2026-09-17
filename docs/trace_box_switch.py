import io, re

HP = r"C:\Users\18352\Desktop\log\http.log"
PL = r"C:\Users\18352\Desktop\log\PLC.log"
T0, T1 = '2026-09-14 08:31', '2026-09-14 09:24'
E5 = ['A10125010200382648014439', 'A10125010200382597320980', 'A10125010201099590154907',
      'A10125010200081468790306', 'A10125000200014629955091']

# ── 1) 格口 017 的容器绑定/换箱时间线（http.log）──
print("=" * 100)
print("① 格口 017 的容器绑定时间线（latticehole=22017）")
print("=" * 100)
binds = []
with io.open(HP, 'r', encoding='utf-8', errors='replace') as f:
    for L in f:
        if 'latticehole=22017' in L or ('切箱' in L and 'latticehole=017' in L):
            ts = L[:19]
            m = re.search(r'boxcode=([A-Za-z0-9\-]+)', L)
            if m:
                binds.append((ts, m.group(1)))
                print(f"  {ts}  → {m.group(1)}")
        if '解绑留痕' in L and 'grid=017' in L:
            ts = L[:19]
            m = re.search(r'旧箱=(\S+)(?: 新箱=(\S+))?', L)
            print(f"  {ts}  [解绑] 旧箱={m.group(1) if m else '?'}"
                  + (f" 新箱={m.group(2)}" if m and m.group(2) else ""))

# ── 2) 5 枚 EPC 的落格时刻（PLC.log，自身 raw 里的格口）──
print("\n" + "=" * 100)
print("② 5 枚 EPC 的落格时刻（PLC 反馈 status=1 → 真实落入格口 017）")
print("=" * 100)
lands = []
with io.open(PL, 'r', encoding='utf-8', errors='replace') as f:
    for L in f:
        ts = L[:19]
        if not (T0 <= ts <= T1): continue
        for e in E5:
            if e in L and 'PLC反馈(5字段)' in L:
                m = re.search(r'raw=\{[^|]*\|(\d+)\|[^|]*\|[^|]*\|(\d)\}', L)
                if m:
                    lands.append((ts, e, m.group(1), m.group(2)))
                    print(f"  {ts}  {e}  格口={m.group(1)}  status={m.group(2)}")
lands.sort()
print(f"  合计 {len(lands)} 条")

# ── 3) 按落格时刻判断当时的容器（取该时刻之前最近一次绑定）──
print("\n" + "=" * 100)
print("③ 逐件归属：落格时刻 → 当时格口 017 绑定的容器")
print("=" * 100)
def box_at(t):
    cur = None
    for ts, b in binds:
        if ts <= t: cur = b
    return cur
by_box = {}
for ts, e, g, st in lands:
    b = box_at(ts)
    by_box.setdefault(b, []).append(e)
    print(f"  {ts}  {e}  → 容器 {b}")
print("\n按容器归并：")
for b, es in by_box.items():
    print(f"  {b}: {len(es)} 件  {es}")

# ── 4) 对照 H7 报文里各容器的上报数量 ──
print("\n" + "=" * 100)
print("④ 对照：该 SKU 在 H7 报文里各容器的上报数量")
print("=" * 100)
with io.open(r"C:\Users\18352\Desktop\log\run.log", 'r', encoding='utf-8', errors='replace') as f:
    for L in f:
        if 'PP202600000587' not in L or '[WMS出站发送]' not in L or 'detailList' not in L: continue
        if '1134010560402' not in L: continue
        ts = L[:19]
        rows = re.findall(r'\{"num":"(\d+)","qty":"(\d+)","sku":"1134010560402","targetLocation":"([^"]+)"\}', L)
        for num, qty, box in rows:
            print(f"  {ts}  报文格口={num} 容器={box}  该SKU qty={qty}")
