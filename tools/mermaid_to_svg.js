// ============================================================================
// mermaid_to_svg.js —— 极简 flowchart(LR/TD) → 独立 SVG 渲染器（零依赖、离线可用）
//
// 为什么自己写：现场机器/本机都无法访问 mermaid CDN（cdn.jsdelivr.net 超时），
//   但流程图要能"双击就打开、断网也能看、能打印"。
//
// 支持范围（只覆盖本项目流程图用到的语法，够用且行为确定）：
//   flowchart LR|TD ;  A[文本<br/>换行] ; B{菱形} ; A --> B ; A -->|标签| B
//   %% @cls A op    —— 本渲染器扩展：给节点指定配色（op/auto/wms/dev/rect/diamond）
//   %% 其它注释、内联样式/class/classDef 一律忽略（统一配色）
//
// 布局：列 = 最长路径（DAG），行 = 前驱行中位数就近占位（无重叠、确定性输出）；
//       回边（指向更早的列，如"未确认重传""取消任务""再次开始接收"）自动走外侧竖向通道。
//
// 用法：node tools/mermaid_to_svg.js <in.txt|in.mmd> <out.svg> [标题] [副标题]
// 退出码：0 = 成功并写出 SVG；1 = 语法/布局错误
// ============================================================================

'use strict';
const fs = require('fs');
const path = require('path');

// 可视宽度：中文/全角按 1 计，ASCII 按 0.55 计（用于折行与节点宽度估算）
const visLen = (t) => [...String(t)].reduce((a, c) => a + (/[\x00-\xff]/.test(c) ? 0.55 : 1), 0);
// 按可视宽度折行（limit 单位为"全角字宽"）
const wrap = (t, limit = 15) => {
  const out = [];
  let cur = '', w = 0;
  for (const c of String(t)) {
    const cw = /[\x00-\xff]/.test(c) ? 0.55 : 1;
    if (w + cw > limit && cur) { out.push(cur); cur = ''; w = 0; }
    cur += c; w += cw;
  }
  if (cur) out.push(cur);
  return out.length ? out : [''];
};

// ─────────────────────────── 1. 解析 ───────────────────────────
function parseMermaid(src) {
  const lines = src.split(/\r?\n/);
  let direction = 'LR';
  const nodes = new Map();   // id -> {id, label:[lines], shape:'rect'|'diamond'}
  const edges = [];          // {from,to,label}
  const clsHints = new Map();// %% @cls 指定
  let header = null;

  const unquote = (s) => {
    s = s.trim();
    if (s.startsWith('"') && s.endsWith('"')) s = s.slice(1, -1);
    return s;
  };
  // 只按显式 <br/> 换行；没有 <br/> 时按可视宽度自动折行（上限 15 个全角字宽）
  const toLines = (s) => {
    const raw = unquote(s);
    if (/<br\s*\/?>/i.test(raw)) return raw.split(/<br\s*\/?>/i).map(t => t.trim()).filter(Boolean);
    return wrap(raw);
  };

  const ensure = (id) => {
    if (!nodes.has(id)) nodes.set(id, { id, label: [id], shape: 'rect' });
    return nodes.get(id);
  };

  const parseNodeRef = (token) => {
    const m = token.trim().match(/^([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[([\s\S]*)\]|\{([\s\S]*)\})?\s*$/);
    if (!m) return null;
    const id = m[1];
    if (m[2] != null) return { id, shape: 'rect', label: toLines(m[2]) };
    if (m[3] != null) return { id, shape: 'diamond', label: toLines(m[3]) };
    return { id, shape: null, label: null };
  };

  for (const raw of lines) {
    let line = raw.trim();
    if (!line || line === '```' || line === '```mermaid') continue;
    if (line.startsWith('%%')) {
      // 本渲染器扩展：%% @cls <nodeId> <op|auto|wms|dev|rect|diamond>
      const cm = line.match(/^%%\s*@cls\s+([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z]+)\s*$/);
      if (cm) clsHints.set(cm[1], cm[2]);
      continue;
    }
    if (line.endsWith(';')) line = line.slice(0, -1).trim();
    if (!line) continue;
    if (!header) {
      const h = line.match(/^(?:flowchart|graph)\s+(LR|RL|TB|TD|BT)$/i);
      if (h) { header = h[0]; direction = h[1].toUpperCase(); continue; }
    }
    if (/^(classDef|class|style|linkStyle|subgraph|end|direction)\b/.test(line)) continue;

    const parts = line.split(/\s*-->\s*/);   // 仅支持 -->（本图足够）
    if (parts.length >= 2) {
      for (let k = 0; k + 1 < parts.length; k++) {
        let fromTok = parts[k], toTok = parts[k + 1], label = null;
        const lm = toTok.match(/^\|([\s\S]*?)\|\s*([\s\S]+)$/);
        if (lm) { label = lm[1].replace(/<br\s*\/?>/gi, ' ').trim(); toTok = lm[2]; }
        const a = parseNodeRef(fromTok), b = parseNodeRef(toTok);
        if (!a || !b) throw new Error(`无法解析的连线：${line}`);
        if (a.shape) { const n = ensure(a.id); n.shape = a.shape; n.label = a.label; }
        if (b.shape) { const n = ensure(b.id); n.shape = b.shape; n.label = b.label; }
        ensure(a.id); ensure(b.id);
        edges.push({ from: a.id, to: b.id, label });
      }
      continue;
    }
    const single = parseNodeRef(line);
    if (single && single.shape) {
      const n = ensure(single.id); n.shape = single.shape; n.label = single.label;
      continue;
    }
    // 其它行（如不含箭头的裸节点声明）忽略
  }

  return { direction, nodes: [...nodes.values()].map(n => ({ ...n, cls: clsHints.get(n.id) || null })), edges };
}

// ─────────────────────────── 2. 布局 ───────────────────────────
// 环检测（DFS）：标记回边 —— 回边走外侧通道，且不参与分层（否则节点会被分到错误的列）
function findFeedbackEdges(model) {
  const succ = new Map(model.nodes.map(n => [n.id, []]));
  for (const e of model.edges) succ.get(e.from).push(e.to);
  const state = new Map();      // 1=在栈上 2=已完成
  const fb = new Set();         // 形如 "from|to"（注意：id 不带引号）
  const dfs = (u) => {
    state.set(u, 1);
    for (const v of succ.get(u)) {
      if (v === u) { fb.add(`${u}|${u}`); continue; }  // 自环
      if (state.get(v) === 1) fb.add(`${u}|${v}`);     // 指向"栈内祖先" → 真正的回边
      else if (!state.has(v)) dfs(v);
    }
    state.set(u, 2);
  };
  for (const n of model.nodes) if (!state.has(n.id)) dfs(n.id);
  return fb;
}

function layout(model) {
  const horiz = model.direction !== 'TD' && model.direction !== 'TB' && model.direction !== 'BT';
  const ids = model.nodes.map(n => n.id);
  const feedback = findFeedbackEdges(model);
  const isBack = (e) => feedback.has(`${e.from}|${e.to}`) || e.from === e.to;

  const succ = new Map(ids.map(i => [i, []]));
  const pred = new Map(ids.map(i => [i, []]));
  for (const e of model.edges) { succ.get(e.from).push(e.to); pred.get(e.to).push(e.from); }

  // 列 = 最长路径
  const col = new Map();
  const depth = (id) => {
    if (col.has(id)) return col.get(id);
    let d = 0;
    // 回边不参与分层（它指向拓扑上更早的节点，否则会把主干压扁）
    for (const e of model.edges) {
      if (e.to !== id || isBack(e)) continue;
      d = Math.max(d, depth(e.from) + 1);
    }
    col.set(id, d);
    return d;
  };
  const depthSafe = (id) => {                          // 防环递归（DFS 回边未覆盖的多重环）
    if (col.has(id)) return col.get(id);
    col.set(id, 0);
    let d = 0;
    for (const e of model.edges) {
      if (e.to !== id || isBack(e)) continue;
      d = Math.max(d, depthSafe(e.from) + 1);
    }
    col.set(id, d);
    return d;
  };
  ids.forEach(depthSafe);

  // ★ 回边会被基础分层压到"同一列"（源与目标同列），渲染时与源节点重叠。
  //   处理：只把"被压平的回边（col(to) <= col(from)）"的目标推到最右列之后；
  //   像 L→B 这种本来就向左回环的回边**保持原位**（推到最右反而看不清回环，且会把起点挪走）。
  const maxCol = Math.max(...ids.map(i => col.get(i)));
  let backSlot = maxCol + 1;
  for (const e of model.edges) {
    if (!isBack(e) || e.from === e.to) continue;
    if (col.get(e.to) === col.get(e.from)) col.set(e.to, backSlot++);   // 被压平才右移
  }

  // 行 = 前驱行中位数就近占位（逐列处理，避免重叠）
  const GAP = 62;                                      // 折行之间的垂直间距（留出边标签的净空）
  const GAP_MIN = 78;                                  // 同一行相邻列的最小水平净间距（留给连线与边标签）
  const LH = 23;                                       // 行高
  // 蛇形折行：每行最多放 maxColsPerRow 列（12 步直链一行到底会拉成 3200px 宽，
  // 屏幕上得横向拖动、打印也不合适；折成 3~4 行后是 1200x400 量级，一屏看得完）
  const maxColsPerRow = Math.max(1, Number(process.env.MMD_MAX_COLS || 0) || 4);
  // 尺寸口径（两轴统一）：主轴长按文字可视宽度 + 内边距；次轴长按行数。
  //   ★ TD 模式必须用同一口径，否则节点宽度会退化成"行数×23"而装不下文字（曾实测漏字）
  const textW = (n) => Math.max(...n.label.map(l => visLen(l))) * 16 + 50;
  const lineH = (n) => n.shape === 'diamond' ? Math.max(90, n.label.length * LH + 52)
                                             : Math.max(52, n.label.length * LH + 30);
  const lineW = (n) => {
    const w = Math.min(460, textW(n));
    return n.shape === 'diamond' ? Math.max(212, w + 16) * 1.25 : Math.max(150, w);
  };
  const colW = [];
  for (const n of model.nodes) {
    const c = col.get(n.id);
    colW[c] = Math.max(colW[c] || 0, lineW(n));
  }
  const colPos = [];                                   // 每"显示列"的中心坐标（蛇形：列号对 maxColsPerRow 取模）
  {
    let a = 0;
    for (let c = 0; c < colW.length; c++) { colPos[c] = a + colW[c] / 2; a += colW[c] + GAP; }
  }
  const dispColOf = (c) => c % maxColsPerRow;
  // 折行后相邻两列可能都很宽（菱形比同列其它节点宽 25%），会出现"左列右边缘压到右列左边缘"。
  // 这里按显示列逐列推进，必要时把整列右移（保持列内对齐，不影响蛇形结构）。
  {
    let cursor = 0;
    for (let c = 0; c < colW.length; c++) {
      const inRow = dispColOf(c);
      if (inRow === 0) cursor = 0;                      // 新的一折从最左开始
      const half = colW[c] / 2;
      if (c === 0 || inRow === 0) { colPos[c] = cursor + half; }
      else {
        const minLeft = colPos[c - 1] + colW[c - 1] / 2 + GAP_MIN;
        colPos[c] = Math.max(cursor + half, minLeft + half);
      }
      cursor = colPos[c] + half;
    }
  }
  const shownCols = Math.min(maxColsPerRow, colW.length);
  const mainExtent = shownCols ? colPos[shownCols - 1] + colW[shownCols - 1] / 2 : 0;

  // 行分配（蛇形折行，参数见上方 maxColsPerRow）
  const rowOf = new Map();
  const rowSizes = [];
  const occupied = new Set();                          // "显示列|行" 占位表（蛇形折行后同格会撞车）
  const maxCol2 = colW.length - 1;
  const ROWS_SOFT = 8;                                 // 软上限：无前驱节点最多排到第 8 行
  for (let c = 0; c <= maxCol2; c++) {
    const group = model.nodes.filter(n => col.get(n.id) === c)
      .sort((a, b) => ids.indexOf(a.id) - ids.indexOf(b.id));
    for (const n of group) {
      const preds = model.edges.filter(e => e.to === n.id && !isBack(e) && rowOf.has(e.from)).map(e => rowOf.get(e.from));
      let r;
      if (preds.length === 1) {
        r = preds[0] + ((c % maxColsPerRow) === 0 ? 1 : 0);   // 到每行首列 → 折到下一行
      } else if (preds.length > 1) {
        const sorted = preds.slice().sort((x, y) => x - y);
        r = sorted[Math.floor((sorted.length - 1) / 2)];
      } else {
        r = Math.min(rowSizes.length, ROWS_SOFT);
      }
      let guard = 0;
      while (occupied.has(`${dispColOf(c)}|${r}`) && guard++ < 64) r++;   // 该显示格已占 → 下移一行
      rowOf.set(n.id, r);
      occupied.add(`${dispColOf(c)}|${r}`);
      rowSizes[r] = Math.max(rowSizes[r] || 0, lineH(n));
    }
  }
  // 约束修正：节点不得高于任一前驱、不得低于任一后继（回边不参与，否则会互相顶出界）
  for (let pass = 0; pass < ids.length + 2; pass++) {
    let changed = false;
    for (const e of model.edges) {
      if (isBack(e) || e.from === e.to) continue;
      const rf = rowOf.get(e.from), rt = rowOf.get(e.to);
      if (rt < rf) { rowOf.set(e.to, rf); changed = true; }
      else if (rt > rf + 1) { rowOf.set(e.to, rf + 1); changed = true; }
    }
    if (!changed) break;
  }
  // 压缩后重算每条行的主轴长度（并消除"空行空洞"：折行后行号可能不连续，
  // 直接按 rowSizes 累积会留下 undefined 间隙，TD 模式曾因此把节点排到画布外）
  const rowSize2 = [];
  for (const n of model.nodes) {
    const r = rowOf.get(n.id);
    rowSize2[r] = Math.max(rowSize2[r] || 0, lineH(n));
  }
  const rowPos = [];
  {
    let a = 0;
    for (let r = 0; r < rowSize2.length; r++) {
      const h = rowSize2[r] || 60;
      rowPos[r] = a + h / 2;
      a += h + GAP;
    }
  }
  const totalCross = rowPos.length ? rowPos[rowPos.length - 1] + (rowSize2[rowSize2.length - 1] || 60) / 2 : 0;

  // 统一映射到 (x=分层方向, y=行方向) 与 (主轴长, 次轴长)，渲染层不再区分方向
  //   · LR/TD 的差别只在两轴互换：LR 主横、TD 主纵；
  //   · contentW/contentH 必须跟着互换，否则 TD 模式下节点会被算到画布外（曾实测）
  const place = new Map();
  for (const n of model.nodes) {
    const c = col.get(n.id), r = rowOf.get(n.id);
    const dc = dispColOf(c);
    const mainLen = colW[c], crossLen = lineH(n);
    const u = colPos[dc], v = rowPos[r];
    place.set(n.id, horiz
      ? { x: u, y: v, w: mainLen, h: crossLen, col: c, row: r, dispCol: dc, shape: n.shape, label: n.label }
      : { x: v, y: u, w: crossLen, h: mainLen, col: c, row: r, dispCol: dc, shape: n.shape, label: n.label });
  }
  return {
    horiz, place, colW, rowPos, rowOf, col, feedback, maxColsPerRow, GAP, GAP_MIN,
    contentW: horiz ? mainExtent : totalCross,
    contentH: horiz ? totalCross : mainExtent,
  };
}

// ─────────────────────────── 3. 渲染 ───────────────────────────
const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const PALETTE = {
  rect:    { fill: '#FFFFFF', stroke: '#37474F', text: '#212121', weight: 1.6 },
  diamond: { fill: '#FFFDE7', stroke: '#F9A825', text: '#795548', weight: 1.8 },
  auto:    { fill: '#F5F5F5', stroke: '#9E9E9E', text: '#212121', weight: 1.4 },
  wms:     { fill: '#E8F5E9', stroke: '#2E7D32', text: '#1B5E20', weight: 1.8 },
  op:      { fill: '#E3F2FD', stroke: '#1565C0', text: '#0D47A1', weight: 2.0 },
  dev:     { fill: '#FFF3E0', stroke: '#EF6C00', text: '#E65100', weight: 1.8 },
};
const FONT = "'Microsoft YaHei','PingFang SC','Segoe UI',sans-serif";

// opt.zoom：放大倍数（默认 1.35，缩放系数只在 <g> 的 transform 上，坐标值保持干净可读）
function renderSVG(model, L, opt = {}) {
  const PAD = 52;
  const zoom = opt.zoom || 1.35;
  const GAP = L.GAP || 62, GAP_MIN = L.GAP_MIN || 78;
  const X0 = PAD + (opt.topPad || 0), Y0 = PAD + (opt.topPad || 0);
  const outW = L.contentW + PAD * 2, outH = L.contentH + PAD * 2;
  const P = L.place;
  const horiz = L.horiz;

  // 主轴方向 a→b 的出口/入口点（+ 次轴偏移 t）
  const exitPt = (n, side, t) => horiz ? [n.x + (side > 0 ? n.w / 2 : -n.w / 2), n.y + (t || 0)]
                                       : [n.x + (t || 0), n.y + (side > 0 ? n.h / 2 : -n.h / 2)];
  const out = [];
  out.push(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${(outW * zoom).toFixed(0)} ${(outH * zoom).toFixed(0)}" width="${(outW * zoom).toFixed(0)}" height="${(outH * zoom).toFixed(0)}" font-family="${FONT}">`);
  out.push(`<rect x="0" y="0" width="${(outW * zoom).toFixed(0)}" height="${(outH * zoom).toFixed(0)}" fill="#FFFFFF"/>`);
  out.push(`<defs>
    <marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path d="M0,0 L10,5 L0,10 z" fill="#546E7A"/>
    </marker>
    <marker id="ah-fb" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path d="M0,0 L10,5 L0,10 z" fill="#FB8C00"/>
    </marker>
  </defs>`);
  out.push(`<style>
    .nl { font-size:15px; font-weight:600; }
    .el { font-size:13px; fill:#37474F; }
    .elb { fill:#FFFFFF; fill-opacity:.95; stroke:#CFD8DC; stroke-width:1; }
    .fb { stroke:#FB8C00; stroke-width:2; fill:none; stroke-dasharray:7 4; }
    .fw { stroke:#546E7A; stroke-width:1.8; fill:none; }
    text { dominant-baseline:middle; }
  </style>`);
  out.push(`<g transform="translate(${X0},${Y0}) scale(${zoom})">`);

  // ── 边（先画线，节点盖在上面）──
  // 标签一律放进"两侧节点之间的净空"里（行间距 / 列间距），并按净空宽度折行，
  // 这样不会压住节点内的文字，也不会跟别的标签重叠。
  const labels = [];
  const gapCenter = (a, b) => horiz
    ? [0, (a.y + a.h / 2 + b.y - b.h / 2) / 2]         // 纵向净空中心
    : [(a.x + a.w / 2 + b.x - b.w / 2) / 2, 0];        // 横向净空中心
  for (const e of model.edges) {
    const a = P.get(e.from), b = P.get(e.to);
    if (!a || !b || e.from === e.to) continue;
    // 蛇形折行后"坐标更大"不等于"更靠后"，故用序列列号判断方向：
    //   目标列更小、或指向同一显示列（要绕过整行）的边，都走外侧通道（虚线）。
    const forward = b.col > a.col && b.dispCol !== a.dispCol;
    const [gapU, gapV] = gapCenter(a, b);              // 两节点之间净空的中心（另一轴为 0）
    let pts, labelAt, feedback = false;
    if (forward) {
      const mid = horiz ? (a.x + a.w / 2 + b.x - b.w / 2) / 2 : (a.y + a.h / 2 + b.y - b.h / 2) / 2;
      const sameCross = Math.abs(horiz ? a.y - b.y : a.x - b.x) < 1;
      if (sameCross) {
        pts = [exitPt(a, 1, 0), exitPt(b, -1, 0)];
        // 标签放在行/列净空里：主轴取两节点之间的中点，次轴取净空中心
        labelAt = horiz ? [mid, gapV] : [gapU, mid];
      } else {
        pts = [exitPt(a, 1, 0),
               horiz ? [mid, a.y] : [a.x, mid],
               horiz ? [mid, b.y] : [b.x, mid],
               exitPt(b, -1, 0)];
        labelAt = horiz ? [mid, (a.y + b.y) / 2] : [(a.x + b.x) / 2, mid];
      }
    } else {
      // 回边：走外侧通道（虚线 + 橙色箭头）
      feedback = true;
      const ch = horiz ? Math.max(12, Math.min(a.x - a.w / 2, b.x - b.w / 2) - 34)
                       : Math.max(12, Math.min(a.y - a.h / 2, b.y - b.h / 2) - 34);
      pts = [exitPt(a, -1, 0),
             horiz ? [ch, a.y] : [a.x, ch],
             horiz ? [ch, b.y] : [b.x, ch],
             exitPt(b, -1, 0)];
      labelAt = horiz ? [(ch + Math.min(a.x - a.w / 2, b.x - b.w / 2)) / 2, gapV || (a.y + b.y) / 2]
                      : [(gapU || (a.x + b.x) / 2), (ch + Math.min(a.y - a.h / 2, b.y - b.h / 2)) / 2];
    }
    const d = pts.map((p, i) => `${i ? 'L' : 'M'}${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' ');
    out.push(`<path class="${feedback ? 'fb' : 'fw'}" marker-end="url(#${feedback ? 'ah-fb' : 'ah'})" d="${d}"/>`);
    if (e.label) labels.push({ text: e.label, x: labelAt[0], y: labelAt[1], a, b });
  }
  // 边标签（白底 + 描边）：先算出所有"行带/列带"及其之间的净空，再把标签摆进净空里
  //   —— 同带边（同一行内的相邻两节点）：标签放该带的上/下净空（同 x 位置）；
  //      跨带边：标签放两带之间的净空中央。
  //   逐个用"候选位置打分"避让（不压节点 > 不压已放标签），保证长标签（如
  //   "超时未确认 报文保留、下次自动补传"）也能落得下、且不遮节点文字。
  const bandKey = (p) => (horiz ? p.y : p.x);
  const bands = [];
  for (const p of P.values()) {
    const key = bandKey(p), size = horiz ? p.h : p.w;
    let b = bands.find(x => Math.abs(x.center - key) < 1);
    if (!b) { b = { center: key, half: size / 2 }; bands.push(b); }
    b.half = Math.max(b.half, size / 2);
  }
  bands.sort((a, b) => a.center - b.center);
  // 每条带上下（左右）的净空区间 [lo, hi]
  const freeZones = horiz
    ? bands.map((b, i) => ({
        above: i === 0 ? null : [bands[i - 1].center + bands[i - 1].half, b.center - b.half],
        below: i === bands.length - 1 ? null : [b.center + b.half, bands[i + 1].center - bands[i + 1].half],
      }))
    : bands.map((b, i) => ({
        above: i === 0 ? null : [bands[i - 1].center + bands[i - 1].half, b.center - b.half],
        below: i === bands.length - 1 ? null : [b.center + b.half, bands[i + 1].center - bands[i + 1].half],
      }));
  const bandIndexOf = (p) => bands.findIndex(b => Math.abs(b.center - bandKey(p)) < 1);
  const nodeRects = [...P.values()].map(p => ({ x1: p.x - p.w / 2, y1: p.y - p.h / 2, x2: p.x + p.w / 2, y2: p.y + p.h / 2 }));
  const overlapArea = (r, s) => Math.max(0, Math.min(r.x2, s.x2) - Math.max(r.x1, s.x1)) *
                               Math.max(0, Math.min(r.y2, s.y2) - Math.max(r.y1, s.y1));
  const placed = [];
  for (const lb of labels) {
    // 尺寸：按"净空厚度 - 6"折行（不会横向溢出净空）
    const maxCross = Math.max(46, horiz ? GAP_MIN : GAP);
    const lines = wrap(lb.text, Math.max(4, Math.floor((maxCross - 10) / 13.2)));
    const w = Math.max(...lines.map(l => visLen(l))) * 13.2 + 12;
    const h = lines.length * 18 + 6;

    // 候选位置
    const cands = [];
    if (lb.a && lb.b) {
      const ia = bandIndexOf(lb.a), ib = bandIndexOf(lb.b);
      const sameBand = ia === ib && ia >= 0;
      if (sameBand) {
        // 同带：放到该带上/下净空
        for (const z of [freeZones[ia].above, freeZones[ia].below]) {
          if (!z) continue;
          const cc = (z[0] + z[1]) / 2;
          cands.push(horiz ? [lb.x, cc] : [cc, lb.y]);
        }
      } else if (ia >= 0 && ib >= 0) {
        const lo = Math.min(ia, ib), hi = Math.max(ia, ib);
        const a = bands[lo].center + bands[lo].half, b2 = bands[hi].center - bands[hi].half;
        const cc = (a + b2) / 2;
        cands.push(horiz ? [lb.x, cc] : [cc, lb.y]);
      }
    }
    // 兜底/补充候选：原位置、以及"线上/四周"的多个偏移位（偏移量按标签尺寸放大，便于让开节点）
    cands.push([lb.x, lb.y]);
    if (horiz) {
      for (const dy of [-1, 1]) {
        cands.push([lb.x, lb.y + dy * (h / 2 + 6)]);
        cands.push([lb.x, lb.y + dy * (h + 14)]);
        cands.push([lb.x - 45, lb.y + dy * (h / 2 + 6)]);
        cands.push([lb.x + 45, lb.y + dy * (h / 2 + 6)]);
      }
    } else {
      for (const dx of [-1, 1]) {
        cands.push([lb.x + dx * (w / 2 + 6), lb.y]);
        cands.push([lb.x + dx * (w + 14), lb.y]);
        cands.push([lb.x + dx * (w / 2 + 6), lb.y - h - 8]);
      }
    }

    let best = null;
    for (const c of cands) {
      const r = { x1: c[0] - w / 2, y1: c[1] - h / 2, x2: c[0] + w / 2, y2: c[1] + h / 2 };
      let cost = 0;
      for (const nr of nodeRects) cost += overlapArea(r, nr) * 2;      // 压节点权重更高
      for (const pr of placed) cost += overlapArea(r, pr) * 3;         // 标签之间最不允许重叠
      if (!best || cost < best.cost) best = { cost, r, at: c };
      if (cost === 0) break;
    }
    placed.push(best.r);
    out.push(`<rect class="elb" x="${best.r.x1.toFixed(1)}" y="${best.r.y1.toFixed(1)}" width="${w.toFixed(1)}" height="${h}" rx="4"/>`);
    lines.forEach((t, i) => {
      out.push(`<text class="el" x="${best.at[0].toFixed(1)}" y="${(best.r.y1 + 12 + i * 18).toFixed(1)}" text-anchor="middle">${esc(t)}</text>`);
    });
  }

  // ── 节点 ──
  const LH = 23;
  for (const n of model.nodes) {
    const p = P.get(n.id);
    const pal = PALETTE[n.cls] || (n.shape === 'diamond' ? PALETTE.diamond : PALETTE.rect);
    const { x, y, w, h } = p;
    const left = (x - w / 2).toFixed(1), top = (y - h / 2).toFixed(1);
    if (n.shape === 'diamond') {
      const pts = `${x.toFixed(1)},${top} ${(x + w / 2).toFixed(1)},${y.toFixed(1)} ${x.toFixed(1)},${(y + h / 2).toFixed(1)} ${(x - w / 2).toFixed(1)},${y.toFixed(1)}`;
      out.push(`<polygon points="${pts}" fill="${pal.fill}" stroke="${pal.stroke}" stroke-width="${pal.weight}"/>`);
    } else {
      const rx = n.cls === 'wms' ? Math.min(24, h / 2 - 2) : 9;
      out.push(`<rect x="${left}" y="${top}" width="${w.toFixed(1)}" height="${h}" rx="${rx}" fill="${pal.fill}" stroke="${pal.stroke}" stroke-width="${pal.weight}"/>`);
    }
    const lines = n.label;
    const start = y - ((lines.length - 1) * LH) / 2;
    lines.forEach((t, i) => {
      const ty = (start + i * LH).toFixed(1);
      out.push(`<text class="nl" x="${x.toFixed(1)}" y="${ty}" text-anchor="middle" fill="${pal.text}">${esc(t)}</text>`);
    });
  }
  out.push('</g></svg>');
  return out.join('\n');
}

// ─────────────────────────── 4. 入口 ───────────────────────────
function main() {
  const [inFile, outFile, title = '流程图'] = process.argv.slice(2);
  if (!inFile || !outFile) {
    console.error('用法: node tools/mermaid_to_svg.js <in.txt|in.mmd> <out.svg> [标题]');
    process.exit(1);
  }
  const model = parseMermaid(fs.readFileSync(inFile, 'utf8'));
  if (!model.nodes.length) { console.error('未解析到任何节点'); process.exit(1); }
  const L = layout(model);
  fs.writeFileSync(outFile, renderSVG(model, L), 'utf8');
  console.log(`✓ ${path.basename(outFile)}: ${model.direction} | 节点 ${model.nodes.length} 个，连线 ${model.edges.length} 条 | 内容 ${Math.round(L.contentW)}x${Math.round(L.contentH)}`);
}

if (require.main === module) main();
module.exports = { parseMermaid, layout, renderSVG, PALETTE, FONT };

