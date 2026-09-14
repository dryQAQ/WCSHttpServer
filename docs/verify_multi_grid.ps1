# ============================================================================
# verify_multi_grid.ps1 — 同品多格口「按计划分配」现场日志核对（只读，不改任何文件）
#
# 用途：波次跑完后，对 release_WcsHttpServer/log 下的日志做一次静态核对，回答四个问题：
#   ① H4 里哪些 SKU 是"同品多格口"、每个格口各计划几件（分配表是否被正确保留）
#   ② 下发时是否按计划分配（选格-按计划分配），有没有仍然"全落第一个格口"
#   ③ 超出计划的件是否被改投异常口（选格-超计划 / 异常口落格）
#   ④ 异常口是否被误上传（H7 里出现异常口编码即不合格）
#
# 用法：
#   pwsh -File docs\verify_multi_grid.ps1 -LogDir release_WcsHttpServer\log
#   pwsh -File docs\verify_multi_grid.ps1 -LogDir <某次备份的日志目录> -Top 20
#
# 说明：日志格式变化时匹配失败会打印"未匹配到"，请把样例日志片段贴回代码侧调整正则。
# ============================================================================
[CmdletBinding()]
param(
    [string] $LogDir = "release_WcsHttpServer\log",
    [int]    $Top    = 15,
    [string] $ExcGrid = "066"      # 异常口内部 key（3 位）；与配置 exceptionGrid 对应
)

$ErrorActionPreference = "Stop"

function Write-Head($t) {
    Write-Host ""
    Write-Host ("=" * 78) -ForegroundColor DarkGray
    Write-Host $t -ForegroundColor Cyan
    Write-Host ("=" * 78) -ForegroundColor DarkGray
}
function Write-Ok($t)   { Write-Host "  [通过] $t" -ForegroundColor Green }
function Write-Bad($t)  { Write-Host "  [失败] $t" -ForegroundColor Red }
function Write-Warn2($t){ Write-Host "  [注意] $t" -ForegroundColor Yellow }
function Write-Info($t) { Write-Host "  $t" }

if (-not (Test-Path $LogDir)) {
    Write-Bad "日志目录不存在：$LogDir"
    exit 1
}

$files = Get-ChildItem -Path $LogDir -Recurse -Include *.log -File -ErrorAction SilentlyContinue
if (-not $files -or $files.Count -eq 0) {
    Write-Bad "日志目录下没有 *.log 文件：$LogDir"
    exit 1
}

# 日志统一按 UTF-8 解码（hlog 输出为 UTF-8）；改用容错解码，避免个别非 UTF-8 字节导致脚本中断
$Utf8Relaxed = New-Object System.Text.UTF8Encoding($false, $false)
Write-Host ("日志目录：{0}（{1} 个日志文件，共 {2:N1} MB，最后写入 {3}）" -f `
    $LogDir, $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB), `
    (($files | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime))

# ── 逐行读取（大文件流式，避免一次性载入内存）─────────────────────────────
$planLines      = New-Object System.Collections.Generic.List[string]   # 同品多格口分配（解析期）
$allocLines     = New-Object System.Collections.Generic.List[string]   # 选格-按计划分配
$overLines     = New-Object System.Collections.Generic.List[string]   # 选格-超计划
$excLandLines   = New-Object System.Collections.Generic.List[string]   # 异常口落格反馈
$warnLines      = New-Object System.Collections.Generic.List[string]   # 超计划-预警
$preLines       = New-Object System.Collections.Generic.List[string]   # 计划格口预检
$wrongGridLines = New-Object System.Collections.Generic.List[string]   # 落格不在计划内（落错格）
$h7NumGrids     = New-Object System.Collections.Generic.HashSet[string] # H7 报文里出现过的 num

$rePlan  = [regex]'\[SKU映射\]\s*同品多格口分配\s+orderCode=(\S+)\s+涉及SKU=(\d+)\s+明细'
$reAlloc = [regex]'选格-按计划分配\s+code=(\S+)\s+映射=\[([^\]]*)\]\s+选中格=(\d+)(?:\(([^)]*)\))?\s+计划=(\d+)件\s+已落=(\d+)件'
$reOver  = [regex]'选格-超计划\s+code=(\S+)\s+映射=\[([^\]]*)\]\s+分配表=(.*?)\s*→\s*(.*)$'
$reExcL  = [regex]'\[异常口\]\s*超计划件已真实落入异常口\s+epc=(\S+)\s+sku=(\S*)\s+grid=(\S+)\s+容器=(\S+)'
$reWarn  = [regex]'\[超计划-预警\]\s*格口(\S+?)(?:\(类型(\S*?)\))?\s+容器(\S+)\s+SKU=(\S+)\s+EPC=(\S+)\s+本格口计划(\d+)件\(SKU总计划(\d+)件\)\s+实际落格(\d+)件\s+多余(\d+)件'
$rePre   = [regex]'计划格口预检 order=(\S+)\s+计划格口=(\d+)（([^）]*)）已绑定=(\d+)\s+未绑定=(\d+)\s+已禁用=(\d+)'
$reWrong = [regex]'落格校验 实际格口不在该SKU计划内\s+code=(\S+)\s+sku=(\S+)\s+实际=(\S+)\s+计划格口=\[([^\]]*)\]'
$reNum   = [regex]'"num"\s*:\s*"(\d+)"'

foreach ($f in $files) {
    $reader = New-Object System.IO.StreamReader($f.FullName, $Utf8Relaxed)
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($line -like '*同品多格口分配*') { if ($rePlan.IsMatch($line))  { $planLines.Add($line) } ; continue }
            if ($line -like '*选格-按计划分配*') { if ($reAlloc.IsMatch($line)) { $allocLines.Add($line) } ; continue }
            if ($line -like '*选格-超计划*')    { if ($reOver.IsMatch($line))  { $overLines.Add($line) } ; continue }
            if ($line -like '*已真实落入异常口*'){ if ($reExcL.IsMatch($line)) { $excLandLines.Add($line) } ; continue }
            if ($line -like '*超计划-预警*')    { if ($reWarn.IsMatch($line))  { $warnLines.Add($line) } ; continue }
            if ($line -like '*计划格口预检*')   { if ($rePre.IsMatch($line))   { $preLines.Add($line) } ; continue }
            if ($line -like '*落格校验*')       { if ($reWrong.IsMatch($line)) { $wrongGridLines.Add($line) } ; continue }
            if ($line -like '*"num"*')          { foreach ($m in $reNum.Matches($line)) { [void]$h7NumGrids.Add($m.Groups[1].Value) } }
        }
    } finally {
        $reader.Close()
    }
}

# ── ⓪ 开工前「计划格口 vs 容器绑定」预检 ─────────────────────────────────
Write-Head "⓪ 开工预检（计划格口是否都有容器可落）"
if ($preLines.Count -eq 0) {
    Write-Warn2 "未匹配到「计划格口预检」日志 —— exe 可能是旧版本（本项为 2026-09-14 新增）"
} else {
    $m = $rePre.Match($preLines[$preLines.Count - 1])
    Write-Ok "计划格口=$($m.Groups[2].Value)（$($m.Groups[3].Value)）已绑定=$($m.Groups[4].Value) 未绑定=$($m.Groups[5].Value) 已禁用=$($m.Groups[6].Value)"
    if ([int]$m.Groups[5].Value -gt 0) {
        Write-Bad "有 $($m.Groups[5].Value) 个计划格口未绑定容器 —— 落到这些格口的件无法进箱，请先让 WMS 下发 H6"
    } else {
        Write-Ok "所有计划格口均有容器绑定"
    }
    if ([int]$m.Groups[6].Value -gt 0) {
        Write-Warn2 "有 $($m.Groups[6].Value) 个计划格口处于满箱未重绑(禁用) —— 其计划件会改分到同 SKU 的其它计划格口"
    }
    $preLines | Select-Object -Last 1 | ForEach-Object { Write-Info ("  " + $_.Trim()) }
}

# ── ① 同品多格口清单（来自解析期分配日志）────────────────────────────────
Write-Head "① H4 同品多格口 SKU 清单（各格口计划件数 + 格口类型）"
$planSku = @{}     # SKU -> @{ order; grids=@( @{grid;qty} ) }
$lastOrder = ""
foreach ($ln in $planLines) {
    $m = $rePlan.Match($ln)
    $lastOrder = $m.Groups[1].Value
    # 明细段直接按 "SKU→[格口(类型):N件+…]" 逐个提取（不使用"第一个冒号"这类脆弱切分：
    # 日志里明细前缀本身带冒号，如 明细(格口(类型):件数):）
    foreach ($sm in [regex]::Matches($ln, '(\S+?)→\[([^\]]*)\]')) {
        $sku = $sm.Groups[1].Value
        # 去掉可能残留的前缀字符（如 "件数): "）
        $sku = ($sku -split '[^\w\-]')[-1]
        if ([string]::IsNullOrEmpty($sku)) { continue }
        $gl  = New-Object System.Collections.Generic.List[object]
        foreach ($g in ($sm.Groups[2].Value -split '\+')) {
            # 兼容两种明细写法：034(分类):1件（含每格口类型） 与旧版 034:1件
            $gm = [regex]::Match($g.Trim(), '^(\d+)(?:\(([^)]*)\))?:(\d+)件$')
            if ($gm.Success) {
                $gt = $gm.Groups[2].Value
                if ([string]::IsNullOrEmpty($gt)) { $gt = "—" }
                $gl.Add([pscustomobject]@{ Grid = $gm.Groups[1].Value; Type = $gt; Qty = [int]$gm.Groups[3].Value })
            }
        }
        if ($gl.Count -gt 0) { $planSku[$sku] = [pscustomobject]@{ Order = $lastOrder; Grids = $gl } }
    }
}
if ($planSku.Count -eq 0) {
    Write-Warn2 "未匹配到「同品多格口分配」日志 —— 本波次可能确实没有多格口 SKU，或 exe 仍是旧版本"
} else {
    Write-Ok "共 $($planSku.Count) 个 SKU 为同品多格口计划"
    $i = 0
    foreach ($kv in ($planSku.GetEnumerator() | Sort-Object Name)) {
        if ($i -ge $Top) { Write-Info "…（其余 $($planSku.Count - $Top) 个见日志）"; break }
        $desc = ($kv.Value.Grids | ForEach-Object { "$($_.Grid)($($_.Type)):$($_.Qty)件" }) -join " + "
        Write-Info ("{0}  合计{1}件  [{2}]" -f $kv.Key, (($kv.Value.Grids | Measure-Object Qty -Sum).Sum), $desc)
        $i++
    }
    $typeCnt = @{}
    foreach ($kv in $planSku.GetEnumerator()) {
        foreach ($g in $kv.Value.Grids) {
            $k = if ($g.Type -eq "2") { "发货" } elseif ($g.Type -eq "1") { "异常" } elseif ($g.Type -eq "0" -or $g.Type -eq "—") { "分类/未标" } else { $g.Type }
            if (-not $typeCnt.ContainsKey($k)) { $typeCnt[$k] = 0 }
            $typeCnt[$k]++
        }
    }
    Write-Info ("其中格口类型分布：" + (($typeCnt.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)个格口" }) -join "  "))
}

# ── ② 按计划分配统计 ────────────────────────────────────────────────────
Write-Head "② 下发期「按计划分配」"
if ($allocLines.Count -eq 0) {
    Write-Warn2 "未匹配到「选格-按计划分配」日志"
} else {
    Write-Ok "按计划分配下发次数：$($allocLines.Count)"
    $perGrid = @{}
    foreach ($ln in $allocLines) {
        $m = $reAlloc.Match($ln)
        $g = $m.Groups[3].Value
        $t = $m.Groups[4].Value
        $k = if ([string]::IsNullOrEmpty($t)) { $g } else { "$g($t)" }
        if (-not $perGrid.ContainsKey($k)) { $perGrid[$k] = 0 }
        $perGrid[$k]++
    }
    Write-Info ("选中格口分布（格口(类型)）：" + (($perGrid.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Key)=$($_.Value)次" }) -join "  "))
    Write-Info "样例："
    $allocLines | Select-Object -First 3 | ForEach-Object { Write-Info ("  " + $_.Trim()) }
}

# ── ③ 超计划改投异常口统计 ──────────────────────────────────────────────
Write-Head "③ 超计划件改投异常口"
if ($overLines.Count -eq 0) {
    Write-Info "本波次无超计划改投（正常：计划件数内足量投递）"
} else {
    Write-Ok "超计划改投下发次数：$($overLines.Count)"
    $overLines | Select-Object -First $Top | ForEach-Object { Write-Info ("  " + $_.Trim()) }
    if ($overLines.Count -gt $Top) { Write-Info "…（其余 $($overLines.Count - $Top) 条）" }
}

Write-Host ""
Write-Host "  异常口真实落格件（已不计已分拣、不进 H7，需人工清出）：" -ForegroundColor Yellow
if ($excLandLines.Count -eq 0) {
    Write-Info "无（本波次没有件落入异常口）"
} else {
    $boxCount = @{}
    foreach ($ln in $excLandLines) {
        $m = $reExcL.Match($ln)
        $b = $m.Groups[4].Value
        if (-not $boxCount.ContainsKey($b)) { $boxCount[$b] = 0 }
        $boxCount[$b]++
    }
    Write-Ok "共 $($excLandLines.Count) 件落入异常口，分布：" 
    foreach ($kv in $boxCount.GetEnumerator()) { Write-Info ("  容器 $($kv.Key) → $($kv.Value) 件") }
    $excLandLines | Select-Object -First $Top | ForEach-Object { Write-Info ("  " + $_.Trim()) }
    if ($excLandLines.Count -gt $Top) { Write-Info "…（其余 $($excLandLines.Count - $Top) 件）" }
}

# ── ④ 异常口误上传检查（关键红线）──────────────────────────────────────
Write-Head "④ 异常口是否被误上传 WMS（红线）"
$excWmsCode = "22" + $ExcGrid.TrimStart('0').PadLeft(3, '0')
$hit = @($h7NumGrids | Where-Object { $_ -eq $excWmsCode -or $_ -eq $ExcGrid -or ($_ -match "\d{3}$" -and $_.Substring($_.Length - 3) -eq $ExcGrid) })
if ($hit.Count -gt 0) {
    Write-Bad "H7 报文里出现异常口编码 $($hit -join ', ') —— 异常口件已上传，WMS 实报可能超计划（会被 [2107632] 整条驳回）"
    Write-Warn2 "请人工核对异常口容器内实物，并检查 exe 是否为本次提交编译（落格反馈识别 + 三处入口跳过 + 报文构建层拦截）"
} else {
    Write-Ok "H7 报文中未出现异常口编码（$excWmsCode / $ExcGrid）"
}
if ($h7NumGrids.Count -gt 0) {
    Write-Info ("H7 报文涉及格口数：{0}" -f $h7NumGrids.Count)
}

# ── ⑤ 真误落预警（非异常口）────────────────────────────────────────────
Write-Head "⑤ 超计划误落预警（计划格口内实落超计划）"
if ($warnLines.Count -eq 0) {
    Write-Ok "无「超计划-预警」—— 计划格口未被超出"
} else {
    Write-Warn2 "共 $($warnLines.Count) 条："
    $warnLines | Select-Object -First $Top | ForEach-Object { Write-Info ("  " + $_.Trim()) }
    if ($warnLines.Count -gt $Top) { Write-Info "…（其余 $($warnLines.Count - $Top) 条）" }
    Write-Info "（成因：件被硬塞进计划格口 / 同 SKU 两件同时在线上；多余件不上传，需现场取出）"
}

# ── ⑥ 落错格（实际格口不在该 SKU 计划内）────────────────────────────────
Write-Head "⑥ 落错格检查（实际格口不在该SKU计划内）"
if ($wrongGridLines.Count -eq 0) {
    Write-Ok "无落错格 —— 所有落格都在该 SKU 的计划格口内（分类口/发货口不同类型不算落错）"
} else {
    Write-Warn2 "共 $($wrongGridLines.Count) 条："
    $wrongGridLines | Select-Object -First $Top | ForEach-Object { Write-Info ("  " + $_.Trim()) }
    if ($wrongGridLines.Count -gt $Top) { Write-Info "…（其余 $($wrongGridLines.Count - $Top) 条）" }
    Write-Info "（处置：异常表已留痕 type=wrong_grid，PLC 报成功仍计已分拣；请核对该件是否需人工取出）"
}

# ── 汇总 ────────────────────────────────────────────────────────────────
Write-Head "汇总"
Write-Info ("多格口SKU数={0}  按计划分配={1}次  超计划改投={2}次  异常口落格={3}件  误落预警={4}条  落错格={5}条  开工预检={6}次" -f `
    $planSku.Count, $allocLines.Count, $overLines.Count, $excLandLines.Count, $warnLines.Count, $wrongGridLines.Count, $preLines.Count)
if ($planSku.Count -gt 0 -and $allocLines.Count -eq 0) {
    Write-Bad "存在多格口 SKU 但没有任何「按计划分配」日志 → exe 可能仍是旧版本，或 gridNumber 字段缺失"
}
Write-Host ""
