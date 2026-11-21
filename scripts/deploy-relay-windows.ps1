# Floral —— 一起听中继部署（Windows Server）
#
# 用法：右键「使用 PowerShell 运行」，或者双击 deploy-relay-windows.cmd
# 脚本会自己申请管理员权限，不会一闪而过。
#
# 只做四件事：放一个 exe、放行端口、注册开机自启、自测一次。
# 不需要装 mosquitto，也不需要装 Node。
#
# 可选参数：
#   -Port 8883                          换端口（同时去云控制台安全组放行）
#   -ExePath C:\x\floral-relay.exe      指定 exe 位置

param(
    [int]$Port = 443,
    [string]$ExePath = "",
    [string]$InstallDir = "C:\FloralRelay"
)

$ErrorActionPreference = 'Stop'

function Say($m) { Write-Host "`n==> $m" -ForegroundColor Cyan }
function Ok($m)  { Write-Host "    $m" -ForegroundColor Green }
function Warn($m){ Write-Host "    $m" -ForegroundColor Yellow }
function Fail($m) {
    Write-Host "`n!!  $m" -ForegroundColor Red
    Write-Host "`n完整记录：$InstallDir\deploy.log" -ForegroundColor Yellow
    exit 1
}

# ── 0. 准备日志目录（在所有检查之前，保证一定有记录）──────────
try { New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null } catch {
    $InstallDir = Join-Path $env:TEMP "FloralRelay"
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
}
$logFile = Join-Path $InstallDir "deploy.log"
try { Start-Transcript -Path $logFile -Append -Force | Out-Null } catch { }

Write-Host "Floral 一起听中继部署" -ForegroundColor White
Write-Host "日志：$logFile" -ForegroundColor DarkGray

# ── 1. 管理员权限：没有就自己提权，而不是直接退出 ──────────────
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "`n没有管理员权限，正在申请..." -ForegroundColor Yellow
    $args = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Port $Port -InstallDir `"$InstallDir`""
    if ($ExePath) { $args += " -ExePath `"$ExePath`"" }
    try {
        Start-Process -FilePath "powershell.exe" -ArgumentList $args -Verb RunAs
        Write-Host "已在新窗口里以管理员身份继续。" -ForegroundColor Green
    } catch {
        Write-Host "`n提权被拒绝。请手动这样做：" -ForegroundColor Red
        Write-Host "  1. 开始菜单搜索 PowerShell → 右键「以管理员身份运行」" -ForegroundColor Yellow
        Write-Host "  2. cd $PSScriptRoot" -ForegroundColor Yellow
        Write-Host "  3. .\deploy-relay-windows.ps1" -ForegroundColor Yellow
        Read-Host "`n按回车关闭"
    }
    try { Stop-Transcript | Out-Null } catch { }
    return
}

try {
    # ── 2. 找 exe ──────────────────────────────────────────────
    if (-not $ExePath) { $ExePath = Join-Path $PSScriptRoot "floral-relay.exe" }
    Say "检查安装文件"
    if (-not (Test-Path $ExePath)) {
        Fail "找不到 floral-relay.exe`n    把它放在脚本旁边（$PSScriptRoot），或者用 -ExePath 指定完整路径"
    }
    $size = [math]::Round((Get-Item $ExePath).Length / 1KB)
    Ok "找到 $ExePath（$size KB）"

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $target = Join-Path $InstallDir "floral-relay.exe"
    Get-Process floral-relay -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    Copy-Item $ExePath $target -Force
    Ok "已复制到 $target"

    # ── 3. 端口 ────────────────────────────────────────────────
    Say "检查端口 $Port"
    Get-ScheduledTask -TaskName "FloralRelay" -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-ScheduledTask -TaskName "FloralRelay" -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    $busy = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
    if ($busy) {
        $names = ($busy | ForEach-Object {
            (Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue).ProcessName
        } | Sort-Object -Unique) -join ', '
        Write-Host "    端口 $Port 被占用：$names" -ForegroundColor Yellow
        if ($names -match 'w3wp|System') {
            Write-Host "    这通常是 IIS，可以先停掉：Stop-Service W3SVC -Force" -ForegroundColor Yellow
        }
        Fail "端口被占用，先释放它，或换端口重跑： -Port 8883"
    }
    Ok "端口 $Port 空闲"

    # ── 4. 防火墙 ──────────────────────────────────────────────
    Say "放行 TCP $Port"
    $rule = "Floral Relay $Port"
    Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -DisplayName $rule -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow | Out-Null
    Ok "防火墙规则已加"

    # ── 5. 开机自启 ────────────────────────────────────────────
    Say "注册开机自启"
    Unregister-ScheduledTask -TaskName "FloralRelay" -Confirm:$false -ErrorAction SilentlyContinue
    $action  = New-ScheduledTaskAction -Execute $target -Argument "$Port" -WorkingDirectory $InstallDir
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $princ   = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                  -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
                  -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName "FloralRelay" -Action $action -Trigger $trigger `
        -Principal $princ -Settings $settings -Description "Floral listen-together relay" | Out-Null
    Ok "计划任务 FloralRelay 已注册（开机启动，崩溃后 1 分钟重启）"

    Start-ScheduledTask -TaskName "FloralRelay"
    Start-Sleep -Seconds 3

    $running = Get-Process floral-relay -ErrorAction SilentlyContinue
    if (-not $running) {
        Write-Host "    进程没起来，看日志：" -ForegroundColor Yellow
        $rl = Join-Path $InstallDir "relay.log"
        if (Test-Path $rl) { Get-Content $rl -Tail 15 | ForEach-Object { Write-Host "      $_" } }
        Fail "中继进程没有运行"
    }
    Ok "中继运行中（pid $($running[0].Id)）"

    # ── 6. 自测 ────────────────────────────────────────────────
    Say "自测：连上去并加入测试房间"
    $reply = ""
    try {
        $client = New-Object System.Net.Sockets.TcpClient
        $client.Connect("127.0.0.1", $Port)
        $stream = $client.GetStream()
        $bytes = [Text.Encoding]::ASCII.GetBytes('{"room":"0123456789abcdef0123456789abcdef"}' + "`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        $reader = New-Object System.IO.StreamReader($stream, [Text.Encoding]::ASCII)
        $reply = $reader.ReadLine()
        $client.Close()
    } catch {
        Write-Host "    连接失败：$($_.Exception.Message)" -ForegroundColor Yellow
    }

    if ($reply -match '"ok"\s*:\s*true') {
        Ok "服务器回应：$reply"
    } else {
        Write-Host "    回应：$reply" -ForegroundColor Yellow
        $rl = Join-Path $InstallDir "relay.log"
        if (Test-Path $rl) { Write-Host "    --- relay.log ---" -ForegroundColor Yellow; Get-Content $rl -Tail 15 }
        Fail "自测没通过"
    }

    # ── 7. 汇总 ────────────────────────────────────────────────
    Say "全部完成"
    Write-Host ""
    Write-Host "  服务      floral-relay.exe（计划任务 FloralRelay，开机自启）" -ForegroundColor White
    Write-Host "  监听      0.0.0.0:$Port" -ForegroundColor White
    Write-Host "  目录      $InstallDir" -ForegroundColor White
    Write-Host "  日志      $InstallDir\relay.log" -ForegroundColor White
    Write-Host ""
    Write-Host "  客户端会优先连 47.108.201.112:$Port" -ForegroundColor Gray
    Write-Host "  外部连不上就检查腾讯云安全组是否放行 TCP $Port" -ForegroundColor Gray
    Write-Host ""
}
catch {
    Write-Host "`n!!  出错：$($_.Exception.Message)" -ForegroundColor Red
    Write-Host "    行号：$($_.InvocationInfo.ScriptLineNumber)" -ForegroundColor Red
    Write-Host "    命令：$($_.InvocationInfo.Line.Trim())" -ForegroundColor DarkGray
    Write-Host "`n完整记录：$logFile" -ForegroundColor Yellow
}
finally {
    try { Stop-Transcript | Out-Null } catch { }
    Write-Host ""
    Read-Host "按回车关闭这个窗口"
}
