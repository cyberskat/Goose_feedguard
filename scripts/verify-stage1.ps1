param(
    [ValidateSet('gcc-debug', 'gcc-release', 'clang-debug', 'clang-release')]
    [string]$Preset = 'gcc-release'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$prefix = if ($Preset.StartsWith('gcc-')) { 'C:\msys64\mingw64' } else { 'C:\msys64\clang64' }
$env:PATH = "$prefix\bin;" + $env:PATH
Push-Location $projectRoot
try {
    & "$prefix\bin\cmake.exe" --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    & "$prefix\bin\cmake.exe" --build --preset $Preset --parallel 2
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    & "$prefix\bin\ctest.exe" --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw 'CTest failed' }
    foreach ($app in 'collector', 'converter', 'book_cpp') {
        & "$projectRoot\build\$Preset\$app.exe"
        if ($LASTEXITCODE -ne 0) { throw "$app failed" }
    }
} finally {
    Pop-Location
}
