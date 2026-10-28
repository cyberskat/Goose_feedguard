param(
    [string]$Goose = 'C:\tools\goose\goose-v2026.3-windows-x64\goose.exe',
    [string]$Clang = 'C:\msys64\clang64\bin\clang.exe',
    [switch]$ReleaseOnly
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = (Split-Path -Parent $Clang) + ';' + $env:PATH
$env:GOOSE_HOME = Join-Path $projectRoot 'build\goose-home'
Push-Location $projectRoot
try {
    & $Goose version
    if ($LASTEXITCODE -ne 0) { throw 'Goose version failed' }
    & $Clang --version
    if ($LASTEXITCODE -ne 0) { throw 'Clang version failed' }
    $modeHeader = Join-Path $projectRoot 'include\goose_measurement_mode.h'
    & $Goose build --cc $Clang -O2 --include $modeHeader -v -o goose/orderbook.exe goose/orderbook.goose
    if ($LASTEXITCODE -ne 0) { throw 'Goose Release build failed' }
    & '.\goose\orderbook.exe' --self-test
    if ($LASTEXITCODE -ne 0) { throw 'Goose Release self-test failed' }
    if ($ReleaseOnly) { return }
    & $Goose build --cc $Clang --debug --include $modeHeader -v -o build/goose-debug/orderbook.exe goose/orderbook.goose
    if ($LASTEXITCODE -ne 0) { throw 'Goose Debug build failed' }
    & '.\build\goose-debug\orderbook.exe' --self-test
    if ($LASTEXITCODE -ne 0) { throw 'Goose Debug self-test failed' }
} finally { Pop-Location }
