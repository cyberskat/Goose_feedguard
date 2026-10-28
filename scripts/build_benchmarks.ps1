$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\clang64\bin;' + $env:PATH
Push-Location $projectRoot
try {
    cmake --preset clang-benchmark
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark configure failed' }
    cmake --build --preset clang-benchmark --target book_cpp unit_tests --parallel 2
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark build failed' }
    ctest --preset clang-benchmark
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark tests failed' }
    & "$PSScriptRoot\build_goose.ps1" -ReleaseOnly *>&1 | Tee-Object -FilePath build/stage7-goose-build.log
    if ($LASTEXITCODE -ne 0) { throw 'Goose benchmark build failed' }
} finally { Pop-Location }
