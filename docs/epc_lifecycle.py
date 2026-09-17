import re, io, os

EPCS = [
    'A10125000200094103327484',
    'A10125010200207496926884',
    'A10125010200207490718691',
]
FILES = [r"C:\Users\18352\Desktop\http.log", r"C:\Users\18352\Desktop\run.log"]

# 分类规则：按出现的关键事件打标签
def classify(line):
    if '[WMS出站发送]' in line or '[WMS出站响应]' in line: return 'H7/WMS报文'
    if 'RFID推送 SKU查询入队' in line:      return '① RFID推送(EPC+小车号到达)'
    if 'RFID绑定查询响应' in line:           return '② RFID绑定查询返回(SKU)'
    if 'RFID绑定查询 解析' in line:          return '② RFID绑定查询返回(SKU)'
    if '已存入EpcCache' in line:             return '③ EPC→SKU 入缓存'
    if 'PLC发送成功' in line:                return '④ 下发PLC(分拣指令)'
    if 'PLC发送未成功' in line:              return '④ 下发PLC失败(拦截)'
    if 'PLC发送超时' in line:                return '④ 下发PLC超时'
    if 'PLC反馈状态异常' in line or '不计入成功分拣' in line: return '⑤ PLC反馈异常(不计件)'
    if 'PLC反馈自动分拣' in line:            return '⑤ PLC反馈落格成功'
    if '在途解除' in line:                   return '⑥ 在途解除'
    if '重扫' in line or '二次上传计时归零' in line: return '⑦ 重扫重投'
    if '异常清理' in line:                   return '⑧ 异常数调整'
    if 'EPC绑定 就绪检查' in line or 'EPC绑定 已发送PLC' in line: return '· 就绪/已发'
    if '未就绪' in line:                     return '· 就绪检查'
    if '记录' in line or 'insertRecord' in line: return '⑨ 落格记录'
    return '· 其他'

for epc in EPCS:
    print("=" * 100)
    print(f"EPC = {epc}")
    print("=" * 100)
    n = 0
    for path in FILES:
        if not os.path.exists(path): continue
        src = os.path.basename(path)
        with io.open(path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                if epc not in line: continue
                n += 1
                ts = line[:23]
                tag = classify(line)
                # 提取关键字段
                car  = re.search(r'carNum=(\d+)', line)
                grd  = re.search(r'grid=(\d+)', line)
                sku  = re.search(r'sku=([A-Za-z0-9]+)', line)
                box  = re.search(r'"targetLocation":"([^"]+)"', line)
                extra = ''
                if car: extra += f" 小车={car.group(1)}"
                if grd: extra += f" 格口={grd.group(1)}"
                if box: extra += f" 容器={box.group(1)}"
                if sku: extra += f" SKU={sku.group(1)}"
                # 关键提示
                flag = ''
                if '未成功' in line: flag = '   <<< 被拦截，未下发'
                if '不计入成功分拣' in line: flag = '   <<< 异常，不计件'
                print(f"  [{src[0:4]}] {ts}  {tag}{extra}{flag}")
    print(f"  —— 共 {n} 条相关日志\n")
