# ============================================================================
# sqlite_native.ps1 — 用项目自带的 release_WcsHttpServer\sqlite3.dll 执行 SQL
#
# 用途：不经 python/外部客户端，直接以原生 sqlite3 C API 操作 WCS 数据库，
#       与 WCS_httpServer.exe 使用同一份 SQLite 实现，行为完全一致。
#
# 用法：
#   . .\tools\sqlite_native.ps1
#   $conn = Open-SqliteDb -Path "D:\...\data\sorting_records.db"
#   Invoke-SqliteQuery  -Conn $conn -Sql "SELECT ... "                   # 只读查询
#   Invoke-SqliteUpdate -Conn $conn -Sql "UPDATE ... "                   # 写（自动开事务）
#   Close-SqliteDb      -Conn $conn
#
# 注：连接参数名用 -Conn（不能用 -Db：PowerShell 内置 -Debug 已占用 Db 别名）
#
# 注意：WCS 运行中会持写连接（WAL），写操作前请先退出 WCS_httpServer.exe。
# ============================================================================

$script:SqliteDll = "D:\WCS\WCSApp\WCS_httpServer\release_WcsHttpServer\sqlite3.dll"

if (-not ("SqliteNative" -as [type]))
{
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class SqliteNative
{
    private const string DLL = "sqlite3.dll";
    public const int SQLITE_OK   = 0;
    public const int SQLITE_ROW  = 100;
    public const int SQLITE_DONE = 101;

    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int sqlite3_open_v2(string filename, out IntPtr db, int flags, IntPtr vfs);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int sqlite3_close_v2(IntPtr db);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int sqlite3_exec(IntPtr db, string sql, IntPtr cb, IntPtr arg, out IntPtr errmsg);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern void sqlite3_free(IntPtr p);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int sqlite3_prepare_v2(IntPtr db, byte[] sql, int n, out IntPtr stmt, IntPtr tail);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int sqlite3_step(IntPtr stmt);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int sqlite3_finalize(IntPtr stmt);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int sqlite3_column_count(IntPtr stmt);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr sqlite3_column_name(IntPtr stmt, int i);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int sqlite3_column_type(IntPtr stmt, int i);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr sqlite3_column_text(IntPtr stmt, int i);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern long sqlite3_column_int64(IntPtr stmt, int i);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern double sqlite3_column_double(IntPtr stmt, int i);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int sqlite3_changes(IntPtr db);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr sqlite3_errmsg(IntPtr db);
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr sqlite3_libversion();
    [DllImport(DLL, CallingConvention = CallingConvention.Cdecl)]
    public static extern int sqlite3_busy_timeout(IntPtr db, int ms);
}
"@ -ReferencedAssemblies "System.Data" -ErrorAction Stop
}

function Import-SqliteDll
{
    param([string]$DllPath = $script:SqliteDll)
    if (-not (Test-Path $DllPath)) { throw "sqlite3.dll 不存在: $DllPath" }
    $dir = Split-Path -Parent $DllPath
    if (-not ($env:PATH -split ';' | Where-Object { $_ -eq $dir })) { $env:PATH = "$dir;$env:PATH" }
    # 预加载锁定 DLL 句柄，保证后续 P/Invoke 解析到这一份
    Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class SqliteDllLoader{[DllImport("kernel32",CharSet=CharSet.Unicode,SetLastError=true)]public static extern IntPtr LoadLibraryW(string p);}' -ErrorAction SilentlyContinue
    [void][SqliteDllLoader]::LoadLibraryW($DllPath)
    return [System.Runtime.InteropServices.Marshal]::PtrToStringAnsi([SqliteNative]::sqlite3_libversion())
}

function Open-SqliteDb
{
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [switch]$ReadOnly,
        [int]$BusyTimeoutMs = 5000
    )
    if (-not (Test-Path $Path)) { throw "数据库不存在: $Path" }
    $flags = if ($ReadOnly) { 0x00000001 } else { 0x00000002 }   # READONLY | READWRITE
    $h = [IntPtr]::Zero
    $rc = [SqliteNative]::sqlite3_open_v2($Path, [ref]$h, $flags, [IntPtr]::Zero)
    if ($rc -ne 0) { throw "打开失败 rc=$rc path=$Path" }
    [void][SqliteNative]::sqlite3_busy_timeout($h, $BusyTimeoutMs)
    return [pscustomobject]@{ Handle = $h; Path = $Path; ReadOnly = [bool]$ReadOnly }
}

function Close-SqliteDb
{
    param([Parameter(Mandatory=$true)][object]$Conn)
    if ($Conn -and $Conn.Handle -ne [IntPtr]::Zero) {
        [void][SqliteNative]::sqlite3_close_v2($Conn.Handle)
        $Conn.Handle = [IntPtr]::Zero
    }
}

function Invoke-SqliteQuery
{
    param([Parameter(Mandatory=$true)][object]$Conn, [Parameter(Mandatory=$true)][string]$Sql)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Sql + [char]0)
    $stmt = [IntPtr]::Zero
    $rc = [SqliteNative]::sqlite3_prepare_v2($Conn.Handle, $bytes, $bytes.Length, [ref]$stmt, [IntPtr]::Zero)
    if ($rc -ne 0) {
        $msg = [System.Runtime.InteropServices.Marshal]::PtrToStringAnsi([SqliteNative]::sqlite3_errmsg($Conn.Handle))
        throw "SQL 编译失败 rc=$rc err=$msg"
    }
    try {
        $n = [SqliteNative]::sqlite3_column_count($stmt)
        $cols = @()
        for ($i = 0; $i -lt $n; $i++) {
            $cols += [System.Runtime.InteropServices.Marshal]::PtrToStringAnsi([SqliteNative]::sqlite3_column_name($stmt, $i))
        }
        $out = New-Object System.Collections.ArrayList
        while (($rc = [SqliteNative]::sqlite3_step($stmt)) -eq [SqliteNative]::SQLITE_ROW) {
            $row = [ordered]@{}
            for ($i = 0; $i -lt $n; $i++) {
                switch ([SqliteNative]::sqlite3_column_type($stmt, $i)) {
                    5 { $v = $null }                                              # NULL
                    1 { $v = [SqliteNative]::sqlite3_column_int64($stmt, $i) }     # INTEGER
                    2 { $v = [SqliteNative]::sqlite3_column_double($stmt, $i) }    # FLOAT
                    default { $v = [System.Runtime.InteropServices.Marshal]::PtrToStringAnsi([SqliteNative]::sqlite3_column_text($stmt, $i)) }
                }
                $row[$cols[$i]] = $v
            }
            [void]$out.Add([pscustomobject]$row)
        }
        if ($rc -ne [SqliteNative]::SQLITE_DONE) {
            $msg = [System.Runtime.InteropServices.Marshal]::PtrToStringAnsi([SqliteNative]::sqlite3_errmsg($Conn.Handle))
            throw "执行中断 rc=$rc err=$msg"
        }
        return $out
    }
    finally { [void][SqliteNative]::sqlite3_finalize($stmt) }
}

function Invoke-SqliteUpdate
{
    param([Parameter(Mandatory=$true)][object]$Conn, [Parameter(Mandatory=$true)][string]$Sql, [switch]$NoTransaction)
    if ($Conn.ReadOnly) { throw "该连接是以 -ReadOnly 打开的，拒绝写操作（防误写）：$($Conn.Path)" }
    $changed = 0
    if (-not $NoTransaction) {
        [void](Invoke-SqliteQuery -Conn $Conn -Sql "BEGIN IMMEDIATE")
    }
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Sql + [char]0)
        $stmt = [IntPtr]::Zero
        $rc = [SqliteNative]::sqlite3_prepare_v2($Conn.Handle, $bytes, $bytes.Length, [ref]$stmt, [IntPtr]::Zero)
        if ($rc -ne 0) {
            $msg = [System.Runtime.InteropServices.Marshal]::PtrToStringAnsi([SqliteNative]::sqlite3_errmsg($Conn.Handle))
            throw "SQL 编译失败 rc=$rc err=$msg"
        }
        try {
            $rc = [SqliteNative]::sqlite3_step($stmt)
            if ($rc -ne [SqliteNative]::SQLITE_DONE) {
                $msg = [System.Runtime.InteropServices.Marshal]::PtrToStringAnsi([SqliteNative]::sqlite3_errmsg($Conn.Handle))
                throw "执行失败 rc=$rc err=$msg"
            }
            $changed = [SqliteNative]::sqlite3_changes($Conn.Handle)
        }
        finally { [void][SqliteNative]::sqlite3_finalize($stmt) }
        if (-not $NoTransaction) { [void](Invoke-SqliteQuery -Conn $Conn -Sql "COMMIT") }
        return $changed
    }
    catch {
        if (-not $NoTransaction) { try { [void](Invoke-SqliteQuery -Conn $Conn -Sql "ROLLBACK") } catch {} }
        throw
    }
}

function Show-SqliteTable
{
    param([Parameter(Mandatory=$true)]$Rows, [string]$Title)
    if ($Title) { Write-Host "--- $Title ---" }
    if (-not $Rows -or $Rows.Count -eq 0) { Write-Host "(0 行)"; return }
    $Rows | Format-Table -AutoSize | Out-String -Width 300 | Write-Host
}

$script:SqliteVersion = Import-SqliteDll
Write-Host "[sqlite_native] 已加载 $script:SqliteDll 版本 $script:SqliteVersion"
