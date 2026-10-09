param(
    [string]$DependencyPrefix,
    [string]$Preset = "headless-tests"
)
$ErrorActionPreference = "Stop"
$orbitRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
Push-Location -LiteralPath $orbitRoot
try {
    $orbitConfigureArgs = @("--preset", $Preset)
    if ($DependencyPrefix) { $orbitConfigureArgs += "-DCMAKE_PREFIX_PATH=$DependencyPrefix" }
    & cmake @orbitConfigureArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }
    & cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
    & ctest --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
} finally {
    Pop-Location
}

