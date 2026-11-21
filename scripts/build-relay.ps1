# 编译一起听中继 + 好友服务：tools\*.cpp -> build\floral-relay.exe
#   .\scripts\build-relay.ps1            （编译）
#   .\scripts\build-relay.ps1 -GenKey    （额外生成 RSA 密钥并打印公钥 JWK）
#
# 产物是单文件静态 exe，服务器上不需要装任何运行库。

param([switch]$GenKey)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$out  = Join-Path $root 'build\floral-relay.exe'

New-Item -ItemType Directory -Force -Path (Join-Path $root 'build') | Out-Null

$gpp = 'g++'
$candidates = @(
    (Join-Path $env:USERPROFILE 'Documents\RedPanda-CPP\mingw64\bin\g++.exe'),
    'C:\mingw64\bin\g++.exe',
    'C:\msys64\mingw64\bin\g++.exe'
)
foreach ($c in $candidates) { if (Test-Path $c) { $gpp = $c; break } }

$sources = @(
    'tools\relay-server.cpp',
    'tools\control.cpp',
    'tools\verify.cpp',
    'tools\sec.cpp',
    'src\json.cpp'
) | ForEach-Object { Join-Path $root $_ }

& $gpp -std=c++17 -O2 -static -static-libgcc -static-libstdc++ `
    -o $out $sources -lws2_32 -lbcrypt -lcrypt32 -lwinhttp
if ($LASTEXITCODE -ne 0) { throw "compile failed (exit $LASTEXITCODE)" }

Write-Host ("BUILD OK -> {0} ({1:N0} KB)" -f $out, ((Get-Item $out).Length / 1KB)) -ForegroundColor Green

if ($GenKey) {
    Write-Host "`n生成 RSA 密钥（私钥用 DPAPI 加密保存在 exe 同目录 keys\server.key）" -ForegroundColor Cyan
    $jwk = & $out --genkey
    Write-Host "`n把下面这行填进 src\friends.cpp 的 kServerPublicKey：" -ForegroundColor Yellow
    Write-Host $jwk -ForegroundColor White
}
