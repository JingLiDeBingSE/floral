# 用 CMake 编译 Floral（MinGW 或 MSVC 都行）
#
#   .\scripts\build-cmake.ps1                  两个版本都编（含/不含一起听）
#   .\scripts\build-cmake.ps1 -Variant std     只编不含一起听的那版
#   .\scripts\build-cmake.ps1 -Generator "Visual Studio 17 2022"   MSVC
#   .\scripts\build-cmake.ps1 -Clean           先清掉构建目录
#
# 产物：build\floral_portable.exe / build\floral_std_portable.exe

param(
    [ValidateSet('both','portable','std')]
    [string]$Variant = 'both',
    [string]$Generator = '',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "找不到 cmake，请先安装 CMake 并加入 PATH"
}

$cfg = Join-Path $root 'build-cmake'
if ($Clean -and (Test-Path $cfg)) { Remove-Item $cfg -Recurse -Force }

function Build-Floral([bool]$noTogether) {
    $tag = if ($noTogether) { 'std' } else { 'portable' }
    $dir = Join-Path $cfg $tag

    $args = @('-S', $root, '-B', $dir, '-DFLORAL_PORTABLE=ON')
    if ($noTogether) { $args += '-DFLORAL_NO_TOGETHER=ON' }
    if ($Generator)  { $args += @('-G', $Generator) }

    Write-Host "`n=== $tag ===" -ForegroundColor Cyan
    & cmake @args
    if ($LASTEXITCODE -ne 0) { throw "configure failed ($tag)" }

    & cmake --build $dir --config Release
    if ($LASTEXITCODE -ne 0) { throw "build failed ($tag)" }
}

if ($Variant -eq 'both' -or $Variant -eq 'portable') { Build-Floral $false }
if ($Variant -eq 'both' -or $Variant -eq 'std')      { Build-Floral $true }

Write-Host "`n产物：" -ForegroundColor Green
Get-ChildItem (Join-Path $root 'build') -Filter 'floral*_portable.exe' |
    Sort-Object Name |
    ForEach-Object { "  {0,-26} {1,7:N1} MB" -f $_.Name, ($_.Length / 1MB) }
