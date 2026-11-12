# 组装 sidecars.zip：node 运行时 + 网易云 / QQ / 一起听 三个本地服务
#   .\build-sidecars.ps1
#
# 产物：build\sidecars.zip（由 src/app.rc 内嵌进 exe）

$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$root  = Split-Path -Parent $here
$stage = Join-Path $env:TEMP ('floral-sidecars-' + [guid]::NewGuid().ToString('N').Substring(0, 8))

try {
    New-Item -ItemType Directory "$stage\netease" -Force | Out-Null
    New-Item -ItemType Directory "$stage\qq" -Force | Out-Null
    New-Item -ItemType Directory "$stage\together" -Force | Out-Null

    # node 运行时
    Copy-Item (Get-Command node).Source "$stage\node.exe"

    # 网易云
    Copy-Item (Join-Path $root 'sidecars\netease-api\server.js')    "$stage\netease\"
    Copy-Item (Join-Path $root 'sidecars\netease-api\package.json') "$stage\netease\"
    Copy-Item (Join-Path $root 'sidecars\netease-api\node_modules') "$stage\netease\node_modules" -Recurse

    # QQ
    Copy-Item (Join-Path $root 'sidecars\qq-api\node_modules') "$stage\qq\node_modules" -Recurse
    if (Test-Path (Join-Path $root 'sidecars\qq-api\package.json')) {
        Copy-Item (Join-Path $root 'sidecars\qq-api\package.json') "$stage\qq\"
    }

    # 一起听（MQTT 中继）
    Copy-Item (Join-Path $root 'sidecars\together-mqtt\together-server.js') "$stage\together\"
    Copy-Item (Join-Path $root 'sidecars\together-mqtt\package.json')       "$stage\together\"
    Copy-Item (Join-Path $root 'sidecars\together-mqtt\node_modules')       "$stage\together\node_modules" -Recurse

    $zip = Join-Path $root 'build\sidecars.zip'
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'build') | Out-Null
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal

    Write-Host ("sidecars.zip = {0:N1} MB" -f ((Get-Item $zip).Length / 1MB)) -ForegroundColor Green
} finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
