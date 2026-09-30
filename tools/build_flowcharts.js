// ============================================================================
// build_flowcharts.js —— 由 tools/现场作业流程图.mmd 生成现场可用的流程图文件
//
// 产物（docs/flowchart/）：
//   WCS现场作业流程图_LR.svg    横向版（屏幕/在线渲染同一套内容）
//   WCS现场作业流程图_LR.html   横向版，自包含 HTML（双击即看，可打印，不依赖 CDN）
//   WCS现场作业流程图_TD.svg    竖向版（打印张贴用，折行更窄更高）
//   WCS现场作业流程图_TD.html   竖向版 HTML
//
// 说明：本机/现场都无法访问 mermaid CDN，故 SVG 由 tools/mermaid_to_svg.js 本地生成；
//   md 里同时保留标准 mermaid 代码块，联网环境仍可直接渲染。
//
// 用法：node tools/build_flowcharts.js [--check]
//   --check 只做结构自检不写文件（等价于 .build_check/flow_check.js）
// ============================================================================

'use strict';
const fs = require('fs');
const path = require('path');
const m = require('./mermaid_to_svg.js');

const ROOT = path.join(__dirname, '..');
const SRC = path.join(__dirname, '现场作业流程图.mmd');
const OUTDIR = path.join(ROOT, 'docs', 'flowchart');

const TITLE = 'WCS 现场作业流程图（开机 → 循环）';
const SUBTITLE = '开机 → 软件自启动 → 等待接收任务 → WMS 下发(H4) → 绑定(人工/H6) → 开始分拣 → 运行日志+效率统计 → 一键满箱回传(H7) → WMS 确认 → 结束任务(H8) → WMS 接收完结 → 再次开始接收任务（循环）';
const CASES = [
  { key: 'LR', cols: 4, zoom: 1.35, topPad: 46, note: '横向版（屏幕查看 / 在线渲染同内容）' },
  { key: 'TD', cols: 3, zoom: 1.5, topPad: 46, note: '竖向版（打印张贴，折行更紧凑）' },
];

function svgToHtml(svg, c) {
  return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>${TITLE}</title>
<style>
  :root { color-scheme: light; }
  body { margin:0; background:#fff; font-family:'Microsoft YaHei','PingFang SC',sans-serif; color:#212121; }
  header { padding:14px 20px 10px; border-bottom:2px solid #1565C0; }
  header h1 { margin:0; font-size:20px; color:#0D47A1; }
  header p { margin:6px 0 0; font-size:12px; line-height:1.7; color:#546E7A; }
  .bar { padding:8px 20px; font-size:12px; color:#607D8B; background:#F5F7FA; border-bottom:1px solid #E0E0E0; }
  .bar b { color:#1565C0; }
  .wrap { padding:16px 20px 32px; overflow:auto; }
  .legend { display:flex; flex-wrap:wrap; gap:14px; padding:10px 20px 0; font-size:12px; color:#37474F; }
  .legend i { display:inline-block; width:14px; height:14px; border-radius:3px; margin-right:5px; vertical-align:-2px; }
  @media print {
    header { border-bottom-color:#ccc; }
    .bar { display:none; }
    .wrap { padding:0; }
    @page { margin:12mm; size:${c.key === 'TD' ? 'A4 portrait' : 'A4 landscape'}; }
  }
</style>
</head>
<body>
<header>
  <h1>${TITLE}</h1>
  <p>${SUBTITLE}</p>
</header>
<div class="bar">版本：2026-09-17 ｜ 软件：WCS_httpServer.exe ｜ 本页<b>离线自包含</b>（无需联网），可直接打印或另存 PDF ｜ 配套文档：docs/WCS现场操作手册.md、docs/WCS现场作业流程图.md</div>
<div class="legend">
  <span><i style="background:#E3F2FD;border:2px solid #1565C0"></i>现场人工操作（须点击）</span>
  <span><i style="background:#F5F5F5;border:1px solid #9E9E9E"></i>软件自动完成</span>
  <span><i style="background:#E8F5E9;border:2px solid #2E7D32"></i>WMS / 上位系统</span>
  <span><i style="background:#FFFDE7;border:2px solid #F9A825"></i>判断 / 等待</span>
  <span><i style="border:2px dashed #FB8C00"></i>回环（虚线箭头）</span>
</div>
<div class="wrap">
${svg}
</div>
</body>
</html>
`;
}

function build({ checkOnly = false } = {}) {
  const srcText = fs.readFileSync(SRC, 'utf8');
  const results = [];
  for (const c of CASES) {
    process.env.MMD_MAX_COLS = String(c.cols);
    // LR 用例：直接解析源文件；TD 用例：把 flowchart LR 换成 TD
    const text = c.key === 'TD' ? srcText.replace(/^flowchart\s+LR\s*$/m, 'flowchart TD') : srcText;
    const model = m.parseMermaid(text);
    const L = m.layout(model);
    const svg = m.renderSVG(model, L, { zoom: c.zoom, topPad: c.topPad });

    // ── 结构自检（与 .build_check/flow_check.js 同口径）──
    const problems = [];
    const boxOf = (p) => ({ x1: p.x - p.w / 2, y1: p.y - p.h / 2, x2: p.x + p.w / 2, y2: p.y + p.h / 2 });
    const list = [...L.place].map(([id, p]) => ({ id, ...p, box: boxOf(p) }));
    const vb = svg.match(/viewBox="([^"]+)"/)[1].split(/\s+/).map(Number);
    for (const n of list) {
      const s = c.zoom;
      if ((n.box.x1 + 52) * s < -1 || (n.box.y1 + 52) * s < -1 ||
          (n.box.x2 + 52) * s > vb[2] + 1 || (n.box.y2 + 52) * s > vb[3] + 1) problems.push(`节点 ${n.id} 超出画布`);
    }
    for (let i = 0; i < list.length; i++) for (let j = i + 1; j < list.length; j++) {
      const a = list[i], b = list[j];
      if (Math.min(a.box.x2, b.box.x2) - Math.max(a.box.x1, b.box.x1) > 0.5 &&
          Math.min(a.box.y2, b.box.y2) - Math.max(a.box.y1, b.box.y1) > 0.5) problems.push(`节点重叠 ${a.id}/${b.id}`);
    }
    const paths = svg.match(/<path class="(fw|fb)"[^>]*>/g) || [];
    const expect = model.edges.filter(e => e.from !== e.to).length;
    if (paths.length !== expect) problems.push(`连线数不符 ${paths.length}/${expect}`);
    const labelRects = [...svg.matchAll(/<rect class="elb" x="([-\d.]+)" y="([-\d.]+)" width="([\d.]+)" height="([\d.]+)"/g)]
      .map(x => ({ x1: +x[1], y1: +x[2], x2: +x[1] + +x[3], y2: +x[2] + +x[4] }));
    for (const lr of labelRects) for (const n of list) {
      const ox = Math.min(lr.x2, n.box.x2) - Math.max(lr.x1, n.box.x1);
      const oy = Math.min(lr.y2, n.box.y2) - Math.max(lr.y1, n.box.y1);
      if (ox > 8 && oy > 8) problems.push(`边标签压住节点 ${n.id}`);
    }
    const vlen = (t) => [...t].reduce((a, ch) => a + (/[\x00-\xff]/.test(ch) ? 0.55 : 1), 0) * 16;
    for (const n of list) {
      const along = L.horiz ? n.w : n.h;
      const usable = n.shape === 'diamond' ? along * 0.72 : along - 6;
      for (const line of n.label) if (vlen(line) > usable) problems.push(`文字溢出节点 ${n.id}: ${line}`);
    }

    results.push({ c, model, L, svg, problems, vb });
    console.log(`${problems.length ? '✗' : '✓'} ${c.key}（每行 ${c.cols} 列，zoom ${c.zoom}）：节点 ${model.nodes.length} 连线 ${model.edges.length}` +
                ` 回边 ${(svg.match(/class="fb"/g) || []).length} 标签 ${labelRects.length} 画布 ${vb[2]}x${vb[3]}` +
                (problems.length ? ` — 问题 ${problems.length}: ${problems.slice(0, 3).join('；')}` : ''));
  }

  const bad = results.filter(r => r.problems.length);
  if (bad.length) { console.error('\n自检未通过，未写出文件'); process.exit(1); }
  if (checkOnly) { console.log('\n自检通过（--check 模式，未写文件）'); return; }

  fs.mkdirSync(OUTDIR, { recursive: true });
  const written = [];
  for (const r of results) {
    const base = `WCS现场作业流程图_${r.c.key}`;
    const p1 = path.join(OUTDIR, `${base}.svg`);
    const p2 = path.join(OUTDIR, `${base}.html`);
    fs.writeFileSync(p1, r.svg, 'utf8');
    fs.writeFileSync(p2, svgToHtml(r.svg, r.c), 'utf8');
    written.push(p1, p2);
  }
  console.log('\n已写出：');
  for (const w of written) console.log(`  ${path.relative(ROOT, w)}  (${(fs.statSync(w).size / 1024).toFixed(1)} KB)`);
}

if (require.main === module) build({ checkOnly: process.argv.includes('--check') });
module.exports = { build };
