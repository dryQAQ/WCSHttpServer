# ============================================================================
# sqlite3 原生批量收尾波次 — 只读取证 + 幂等更正
#
# 用法：
#   . .\tools\sqlite_native.ps1
#   .\tools\run_sqlite_fix.ps1
#
# 作用（使用 release_WcsHttpServer\sqlite3.dll 原生 API，不经 python/外部客户端）：
#   1) 只读取证：目标波次在两库的当前状态 + H8 完结回传是否真成功
#   2) 幂等更正：return_wave.status 7→8，同步 wave_history.status
#   3) 复核：未完结面板查询 / 重启恢复查询 / 状态分布
# ============================================================================

. "D:\WCS\WCSApp\WCS_httpServer\tools\sqlite_native.ps1"

$DataDir = "D:\WCS\WCSApp\WCS_httpServer\release_WcsHttpServer\data"
$MainDb  = Join-Path $DataDir "sorting_records.db"
$HistDb  = Join-Path $DataDir "wave_history.db"

# 目标波次 + H8 回传成功的真实时刻（作为终态 updated_at / end_time，不用当下时间）
$Targets = @(
    @{ OrderCode = "PP2026MOC44444"; FinishedAt = "2026-09-15 19:15:43.446" }
)

# ── 0. 前置检查：WCS 必须已退出（运行中会持写连接，写入会被阻塞或丢失）
$proc = Get-Process -Name "WCS_httpServer" -ErrorAction SilentlyContinue
if ($proc) {
    Write-Warning "检测到 WCS_httpServer.exe 正在运行（PID $($proc.Id -join ','))，写库会失败或被覆盖。请先退出 WCS 再执行。"
    return
}
Write-Host "[前置] WCS_httpServer.exe 未运行，可以安全写库`n"

$main  = Open-SqliteDb -Path $MainDb
$hist  = Open-SqliteDb -Path $HistDb

try
{
    foreach ($t in $Targets)
    {
        $oc = $t.OrderCode
        Write-Host "================ 波次 $oc ================"

        # ── 1. 只读取证
        Show-SqliteTable (Invoke-SqliteQuery -Conn $main -Sql @"
SELECT w.order_code, w.order_qty, w.status,
       (SELECT COUNT(*) FROM outbox_end e
         WHERE e.order_code = w.order_code AND e.status = 'success') AS h8_success,
       w.created_at, w.updated_at
FROM return_wave w WHERE w.order_code = '$oc'
"@) "1) 主库改前状态 + H8 成功条数"

        $h8 = (Invoke-SqliteQuery -Conn $main -Sql "SELECT COUNT(*) AS c FROM outbox_end WHERE order_code='$oc' AND status='success'").c
        if ($h8 -le 0) {
            Write-Warning "  该波次没有 H8 成功记录！置 8 会让 WMS 永久缺完结报文，正确做法是置 7 让 Outbox 重试补发 —— 已跳过。"
            continue
        }

        # ── 2. 主库：7 → 8（幂等：已终态则 0 行）
        $changed = Invoke-SqliteUpdate -Conn $main -Sql @"
UPDATE return_wave
   SET status = 8, updated_at = '$($t.FinishedAt)'
 WHERE order_code = '$oc' AND status <> 8
"@
        Write-Host "2) 主库 UPDATE 影响行数: $changed"
        Show-SqliteTable (Invoke-SqliteQuery -Conn $main -Sql "SELECT order_code, order_qty, status, created_at, updated_at FROM return_wave WHERE order_code='$oc'") "   改后"

        # ── 3. 历史库：同步快照状态位（事实值 total_items/sorted 等保持不动）
        $hChanged = Invoke-SqliteUpdate -Conn $hist -Sql @"
UPDATE wave_history
   SET status = 8, status_text = '已完成'
 WHERE order_code = '$oc' AND status <> 8
"@
        Write-Host "3) 历史库 UPDATE 影响行数: $hChanged"
        Show-SqliteTable (Invoke-SqliteQuery -Conn $hist -Sql "SELECT order_code, status, status_text, end_time, total_items, sorted_count, exception_count, end_result FROM wave_history WHERE order_code='$oc'") "   改后"
    }

    # ── 4. 复核
    Write-Host "================ 复核 ================"
    Show-SqliteTable (Invoke-SqliteQuery -Conn $main -Sql "SELECT status, COUNT(*) AS cnt FROM return_wave GROUP BY status ORDER BY status") "状态分布（期望 7=5 → 8=26）"

    Show-SqliteTable (Invoke-SqliteQuery -Conn $main -Sql @"
SELECT w.order_code, w.status FROM return_wave w
WHERE w.status NOT IN (6, 8)
   OR EXISTS (SELECT 1 FROM outbox_end     e WHERE e.order_code = w.order_code AND e.status <> 'success')
   OR EXISTS (SELECT 1 FROM outbox_fullbox f WHERE f.order_code = w.order_code AND f.status <> 'success')
ORDER BY w.updated_at DESC
"@) "未完结面板查询（目标波次不应出现）"

    Show-SqliteTable (Invoke-SqliteQuery -Conn $main -Sql "SELECT order_code, status, updated_at FROM return_wave WHERE status NOT IN (6,8) ORDER BY updated_at DESC LIMIT 1") "重启恢复查询（最新未完成波次）"

    # ── 5. 合并 WAL，让改动落进主 .db 文件
    Show-SqliteTable (Invoke-SqliteQuery -Conn $main -Sql "PRAGMA wal_checkpoint(TRUNCATE)") "主库 WAL checkpoint"
    Show-SqliteTable (Invoke-SqliteQuery -Conn $hist -Sql "PRAGMA wal_checkpoint(TRUNCATE)") "历史库 WAL checkpoint"
}
finally
{
    Close-SqliteDb -Conn $main
    Close-SqliteDb -Conn $hist
    Write-Host "`n[sqlite_native] 连接已关闭"
}
