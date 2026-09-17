import io, re

HP  = r"C:\Users\18352\Desktop\log\http.log"
PL  = r"C:\Users\18352\Desktop\log\PLC.log"
RL  = r"C:\Users\18352\Desktop\log\run.log"
T0, T1 = '2026-09-14 08:31', '2026-09-14 09:24'
SKU = '106101134113101'
E4  = ['A10126010200451784165207', 'A10126010200451793940640',
       'A10126010200451800821384', 'A10126010200451932756193']

# ── 1) H4 计划：该 SKU 的全部计划行 ──
h4 = None
with io.open(RL, 'r', encoding='utf-8', errors='replace') as f:
    for L in f:
        if 'PP202600000587' not in L or 'WAVE_ITEM' not in L or 'body=' not in L: continue
        b = L[L.index('body=') + 5:].strip()
        if h4 is None or len(b) > len(h4): h4 = b
import json
o, _ = json.JSONDecoder().raw_decode(h4.lstrip())
print("=" * 100)
print(f"① H4 计划中 SKU={SKU} 的全部计划行")
print("=" * 100)
for it in o['items']:
    if str(it.get('inco', '')).strip() == SKU:
        print(f"   inco={it['inco']}  gridNum={it['gridNum']}  gridNumber={it['gridNumber']}  gridType={it.get('gridType')}")

# ── 2) 4 枚 EPC 的下发目标格口（WCS 决定送给哪个格口）──
print("\n" + "=" * 100)
print("② 4 枚 EPC 的『下发指令目标格口』（PLC.log 的 cmd={EPC|格口|小车}）")
print("=" * 100)
for e in E4:
    with io.open(PL, 'r', encoding='utf-8', errors='replace') as f:
        for L in f:
            if e not in L: continue
            if 'cmd={' in L:
                m = re.search(r'cmd=\{([^}]*)\}', L)
                ts = L[:19]
                print(f"   {ts}  {e}  → 指令 {m.group(1) if m else '?'}")
# 选格日志（说明为什么选这个格口）
print("\n=== 选格决策日志（PlcManager 选格 code=... 映射=[...] 选中格=... 原因=...）===")
for e in E4:
    with io.open(PL, 'r', encoding='utf-8', errors='replace') as f:
        for L in f:
            if e in L and '选格 code=' in L:
                print("  ", L[:19], L[L.index('选格 code='):L.index('选格 code=') + 180])

# ── 3) 落格反馈 ──
print("\n" + "=" * 100)
print("③ 4 枚 EPC 的落格反馈（终局）")
print("=" * 100)
for e in E4:
    with io.open(PL, 'r', encoding='utf-8', errors='replace') as f:
        for L in f:
            ts = L[:19]
            if not (T0 <= ts <= T1): continue
            if e in L and 'PLC反馈(5字段)' in L:
                m = re.search(r'raw=\{([^}]*)\}', L)
                print(f"   {ts}  {e}  raw={{{m.group(1)}}}")
