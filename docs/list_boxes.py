import io, re, json

RL = r"C:\Users\18352\Desktop\log\run.log"
WAVE = "PP202600000587"

# 取该波次全部 H7 报文中，容器为 H-T0037 / H-T0131 的报文，列出完整明细
targets = {'H-T0037': [], 'H-T0131': []}
with io.open(RL, 'r', encoding='utf-8', errors='replace') as f:
    for L in f:
        if WAVE not in L or '[WMS出站发送]' not in L or 'detailList' not in L:
            continue
        ts = L[:19]
        try:
            b = L[L.index('body=') + 5:].strip()
            b = b[:b.rindex('}') + 1]
            d = json.loads(b)
            dl = d['head']['detailList']
            box = dl[0].get('targetLocation', '')
            if box in targets:
                num = dl[0].get('num', '')
                rows = [(str(x['sku']).strip(), int(str(x['qty'])))
                        for x in dl if str(x.get('targetLocation', '')) == box]
                targets[box].append((ts, num, rows))
        except Exception:
            pass

for box in ('H-T0037', 'H-T0131'):
    msgs = targets[box]
    print("=" * 96)
    print(f"容器 {box}：本波次共 {len(msgs)} 条 H7 报文")
    print("=" * 96)
    for ts, num, rows in msgs:
        tot = sum(q for _, q in rows)
        print(f"\n  【{ts}  格口 {num}】{len(rows)} 个 SKU 行，合计 {tot} 件")
        for sku, q in rows:
            mark = ""
            if sku == '106101134113101':
                mark = "   ★★ 就是这一件（本箱的 4 件之一）"
            elif sku == '1134010560402':
                mark = "   ★★ 就是这个 SKU（本箱 1 件）"
            print(f"      {sku:<20} qty={q}{mark}")
    print()
