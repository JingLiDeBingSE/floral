# 打包 Floral
#
#   .\scripts\pack.ps1          普通版（不含一起听）→ Floral-便携版.zip
#   .\pack.ps1 -NoTogether      同上
#   .\scripts\pack.ps1 -Together 特供版（含一起听）→ Floral-特供版.zip（自用）
#
# 包里只有 floral.exe + 使用说明.txt，不含任何登录状态 / 歌单 / 缓存。

param(
    [switch]$NoTogether,
    [switch]$Together
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$proj = Split-Path -Parent $here

# 默认打普通版；显式 -Together 才打特供版
$withTogether = $Together.IsPresent -and -not $NoTogether.IsPresent

$zip = Join-Path $proj ($(if ($withTogether) { 'Floral-特供版.zip' } else { 'Floral-便携版.zip' }))
$buildArgs = @('-Portable')
if (-not $withTogether) { $buildArgs += '-NoTogether' }

Push-Location $here
try {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'build.ps1') @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
} finally {
    Pop-Location
}

$built = Join-Path $proj ($(if ($withTogether) { 'build\floral_portable.exe' } else { 'build\floral_std_portable.exe' }))
if (-not (Test-Path $built)) { throw "找不到 $built" }

# 打包目录
$stage = Join-Path $proj 'dist'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage | Out-Null
Copy-Item $built (Join-Path $stage 'floral.exe')

$note = if ($withTogether) {
    "  顶栏耳麦按钮   「一起听」（特供版，仅在提供的包里有）`r`n"
} else { "" }

$readme = @"
Floral（便携版）
==============================

双击 floral.exe 即可运行，不需要安装任何东西。

怎么用
------
1. 双击 floral.exe
2. 第一次会弹出「选择要登录的平台」，选一个：
     酷狗音乐    用酷狗 App 扫码
     网易云音乐  用网易云音乐 App 扫码
     QQ 音乐     用 QQ 音乐 App 扫码
3. 手机确认后，程序自动拉「我喜欢的音乐」，列表就出来了
4. 点任意一张封面 → 进入播放页；左上「< 返回」或 Esc 回到列表

界面操作
--------
  点封面        播放这首歌
  点顶栏 ◉      进入正在播放的歌
  拖进度条      跳转（流媒体要等下载追上，会转圈）
  音量按钮      滑杆调音量
  音质          标准 / 高品 / 无损，不可用的档位会标灰
$note  设置（齿轮）  清空缓存 / 重新登录

三家接口服务（含 Node 运行时）全部内嵌在 exe 里，开箱即用；
首次用网易云 / QQ 时会先解压到 %LOCALAPPDATA%\KugouPlayer\sidecars（约 130 MB，几秒钟）。

本目录里应该只有
----------------
floral.exe        主程序
使用说明.txt      本文件

运行时自动生成（不属于安装包）
------------------------------
kugou-session.json / netease-session.json / qq-session.json   登录状态
kugou-likes.tsv / netease-likes.tsv / qq-likes.tsv             歌单数据
covers\                                                         封面缓存
%LOCALAPPDATA%\KugouPlayer\floral-log.txt                       运行日志（每次启动重写）

想换账号：设置 → 重新登录（会先清空缓存和登录信息，再选平台扫码）。

常见问题
--------
· 顶栏显示「登录已过期」
  → 设置 → 重新登录。

· 顶栏显示「接口服务未启动」
  → 杀毒软件可能拦了 %LOCALAPPDATA%\KugouPlayer 里的组件，把该目录加白名单。

· 网易云一直 502 / 提示网络环境异常
  → 网易云有 IP 风控，短时间内请求太多会被限流，等十几分钟再试。

· 程序出问题
  → 把 %LOCALAPPDATA%\KugouPlayer\floral-log.txt 发过来，里面有每一步的记录。
"@
[System.IO.File]::WriteAllText((Join-Path $stage '使用说明.txt'), $readme,
                               (New-Object System.Text.UTF8Encoding($true)))

if (Test-Path $zip) { Remove-Item $zip -Force }
Rename-Item $stage 'Floral'
try {
    Compress-Archive -Path (Join-Path $proj 'Floral') -DestinationPath $zip -CompressionLevel Optimal
} finally {
    Rename-Item (Join-Path $proj 'Floral') 'dist'
}

Write-Host ("打包完成: {0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB)) -ForegroundColor Green
Write-Host ("版本: {0}" -f $(if ($withTogether) { '特供版（含一起听）' } else { '普通版（不含一起听）' })) -ForegroundColor DarkGray
