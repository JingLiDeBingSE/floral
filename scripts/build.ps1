# 编译 Floral（C++ / Win32 + Direct2D）
#
#   从仓库根目录执行：
#     .\scripts\build.ps1                       开发版
#     .\scripts\build.ps1 -Run                  编译并启动
#     .\scripts\build.ps1 -Render shot.png      离屏渲染一张 PNG
#     .\scripts\build.ps1 -Portable             便携版（静态链接 + 内嵌接口服务）
#     .\scripts\build.ps1 -Portable -NoTogether 便携版，不含「一起听」（对外发布）
#
# 输出（都在 build\ 下）：
#   floral.exe              开发版
#   floral_portable.exe     便携版（含一起听）
#   floral_std_portable.exe 便携版（不含一起听）
#
# 依赖：MinGW-w64 g++（含 windres）。没有接口服务 exe 时也能编译，只是不内嵌。

param(
    [switch]$Run,
    [switch]$Portable,
    [switch]$NoTogether,
    [string]$Render
)

$ErrorActionPreference = 'Stop'
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$root   = Split-Path -Parent $here
$srcDir = Join-Path $root 'src'
$outDir = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$name = 'floral'
if ($NoTogether) { $name = 'floral_std' }
if ($Portable)   { $name = $name + '_portable' }
$exe = Join-Path $outDir ($name + '.exe')

Push-Location $srcDir
try {
    # 1) 资源：酷狗接口服务 + 网易云/QQ/一起听 的 sidecars
    $apiExe = Join-Path $root 'sidecars\kugou-api\bin\app_win.exe'
    $res = $null
    if (Test-Path $apiExe) {
        $res = Join-Path $outDir 'res.o'
        & windres app.rc -O coff -o $res
        if ($LASTEXITCODE -ne 0) { throw "windres failed" }
        Write-Host ("resource: {0:N1} MB" -f ((Get-Item $res).Length / 1MB)) -ForegroundColor DarkGray
    } else {
        Write-Host "warn: 找不到 sidecars\kugou-api\bin\app_win.exe，不内嵌接口服务" -ForegroundColor Yellow
    }

    # 2) 链接
    $src = @('main.cpp','together.cpp','log.cpp','theme.cpp','data.cpp','audio.cpp','kugou.cpp','lang.cpp')
    if ($res) { $src += $res }
    $libs = @('-ld2d1','-ldwrite','-lwindowscodecs','-lole32','-luuid','-lshell32',
              '-lgdi32','-ldwmapi','-lwinmm','-lwininet','-lws2_32','-liphlpapi')

    $gpp = @('-std=c++17','-O2','-municode','-mwindows')
    if ($NoTogether) { $gpp += '-DFLORAL_NO_TOGETHER' }
    if ($Portable)   { $gpp += @('-static','-static-libgcc','-static-libstdc++') }
    $gpp += @('-o', $exe) + $src + $libs

    & g++ @gpp
    if ($LASTEXITCODE -ne 0) { throw "compile failed (exit $LASTEXITCODE)" }
    Write-Host ("BUILD OK -> {0} ({1:N1} MB)" -f $exe, ((Get-Item $exe).Length / 1MB)) -ForegroundColor Green
} finally {
    Pop-Location
}

if ($Run) {
    Start-Process -FilePath $exe -WorkingDirectory $root
}

if ($Render) {
    $png = Join-Path $root $Render
    Start-Process -FilePath $exe -ArgumentList @('--render', $png) `
        -WorkingDirectory $root -Wait -NoNewWindow | Out-Null
    if (Test-Path $png) {
        Write-Host ("RENDER OK -> {0} ({1:N0} KB)" -f $png, ((Get-Item $png).Length / 1KB)) -ForegroundColor Green
    } else {
        Write-Host "RENDER FAILED" -ForegroundColor Red
    }
}
