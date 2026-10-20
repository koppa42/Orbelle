param(
    [string]$Executable,
    [string]$DependencyPrefix,
    [string[]]$DesktopArguments = @()
)
$ErrorActionPreference = 'Stop'
$orbitRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $Executable) {
    $Executable = Join-Path $orbitRoot 'out/build/desktop-graphics/apps/desktop/Release/orbitforge_desktop.exe'
}
if (-not $DependencyPrefix) {
    $DependencyPrefix = Join-Path $orbitRoot 'out/dependencies/installed/x64-windows'
}
$orbitExecutable = (Resolve-Path -LiteralPath $Executable).Path
$orbitDependencies = (Resolve-Path -LiteralPath $DependencyPrefix).Path
$orbitStart = [System.Diagnostics.ProcessStartInfo]::new($orbitExecutable)
$orbitStart.UseShellExecute = $false
$orbitStart.WorkingDirectory = Split-Path -Parent $orbitExecutable
$orbitStart.Environment['PATH'] = (Join-Path $orbitDependencies 'bin') + [IO.Path]::PathSeparator + $env:PATH
$orbitStart.Environment['QT_QPA_PLATFORM_PLUGIN_PATH'] = Join-Path $orbitDependencies 'Qt6/plugins/platforms'
$orbitStart.Environment['QT_PLUGIN_PATH'] = Join-Path $orbitDependencies 'Qt6/plugins'
$orbitStart.Environment['QT_QPA_PLATFORM'] = 'windows'
$orbitStart.Environment['VK_LAYER_PATH'] = Join-Path $orbitDependencies 'bin'
$orbitStart.Environment['PROJ_DATA'] = Join-Path $orbitDependencies 'share/proj'
$orbitStart.Environment['GDAL_DATA'] = Join-Path $orbitDependencies 'share/gdal'
$orbitStart.Environment['PROJ_NETWORK'] = 'OFF'
foreach ($orbitArgument in $DesktopArguments) {
    $orbitStart.ArgumentList.Add($orbitArgument)
}
# These paths apply only to the child application; the caller's environment is unchanged.
$orbitProcess = [System.Diagnostics.Process]::Start($orbitStart)
try {
    $orbitProcess.WaitForExit()
    if ($orbitProcess.ExitCode -ne 0) {
        throw "OrbitForge exited with code $($orbitProcess.ExitCode)"
    }
} finally {
    $orbitProcess.Dispose()
}
