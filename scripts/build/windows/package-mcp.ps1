param(
    [Parameter(Mandatory = $true)]
    [string]$GameDirectory,
    [string]$BuildDirectory = 'build-mcp-vcpkg',
    [string]$PackageDirectory = 'build-mcp-realtime-package',
    [string]$GraphicsRuntimeDirectory
)
$ErrorActionPreference = 'Stop'
$repoDirectory = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
$assetDirectory = (Resolve-Path -LiteralPath $GameDirectory).Path
if (-not (Test-Path -LiteralPath $assetDirectory -PathType Container)) {
    throw 'GameDirectory must be the existing Zero Hour/Contra asset directory.'
}
if (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoDirectory $BuildDirectory
}
$builtExecutable = Join-Path $BuildDirectory 'GeneralsMD\Release\GeneralsXZH.exe'
if (-not (Test-Path -LiteralPath $builtExecutable -PathType Leaf)) {
    throw 'Build z_generals in Release with RTS_ENABLE_MCP_BRIDGE=ON before packaging.'
}
$graphicsFiles = @()
if ($GraphicsRuntimeDirectory) {
    $runtimeDirectory = (Resolve-Path -LiteralPath $GraphicsRuntimeDirectory).Path
    foreach ($library in @('d3d8.dll', 'd3d9.dll')) {
        $file = Join-Path $runtimeDirectory $library
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw 'GraphicsRuntimeDirectory must contain the x86 Direct3D 8 and 9 runtime DLLs.'
        }
        $graphicsFiles += $file
    }
}
if (-not [System.IO.Path]::IsPathRooted($PackageDirectory)) {
    $PackageDirectory = Join-Path $repoDirectory $PackageDirectory
}
$packageDirectory = [System.IO.Path]::GetFullPath($PackageDirectory)
New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null
$packagedExecutable = Join-Path $packageDirectory 'GeneralsXZH-MCP.exe'
Copy-Item -LiteralPath $builtExecutable -Destination $packagedExecutable -Force
$vcpkgRuntime = Join-Path $BuildDirectory 'vcpkg_installed\x86-windows\bin'
if (Test-Path -LiteralPath $vcpkgRuntime -PathType Container) {
    Get-ChildItem -LiteralPath $vcpkgRuntime -Filter '*.dll' -File |
        Copy-Item -Destination $packageDirectory -Force
}
Get-ChildItem -LiteralPath (Split-Path -Parent $builtExecutable) -Filter '*.dll' -File |
    Where-Object { $_.Name -notin @('binkw32.dll', 'mss32.dll') } |
    Copy-Item -Destination $packageDirectory -Force
foreach ($file in $graphicsFiles) {
    Copy-Item -LiteralPath $file -Destination $packageDirectory -Force
}
foreach ($runtimeName in @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll')) {
    $runtimeFile = Join-Path $assetDirectory $runtimeName
    if (Test-Path -LiteralPath $runtimeFile -PathType Leaf) {
        $packagedRuntime = Join-Path $packageDirectory $runtimeName
        $runtimeMatches = (Test-Path -LiteralPath $packagedRuntime -PathType Leaf) -and
            ((Get-FileHash -LiteralPath $runtimeFile -Algorithm SHA256).Hash -eq
             (Get-FileHash -LiteralPath $packagedRuntime -Algorithm SHA256).Hash)
        if (-not $runtimeMatches) {
            Copy-Item -LiteralPath $runtimeFile -Destination $packagedRuntime -Force
        }
    }
}
$launcherTemplate = @'
@echo off
setlocal
set "GENERALS_MCP_PIPE=local"
set "GENERALS_MCP_TEST_MAP="
set "GENERALS_MCP_TEST_USER_DATA="
set "GENERALS_MCP_TEST_SESSION="
set "GENERALS_MCP_TEST_WINDOWED="
"%~dp0GeneralsXZH-MCP.exe" -win -quickstart __AUDIO_FLAG__ -setCwd "__ASSET_DIRECTORY__" -assetPath "__ASSET_DIRECTORY__"
'@
# Percent signs are escaped because these paths are written to a batch file.
$launcher = $launcherTemplate.Replace('__ASSET_DIRECTORY__', $assetDirectory.Replace('%', '%%'))
Set-Content -LiteralPath (Join-Path $packageDirectory 'launch-contra.cmd') -Value $launcher.Replace('__AUDIO_FLAG__', '') -Encoding Ascii
Set-Content -LiteralPath (Join-Path $packageDirectory 'launch-contra-noaudio.cmd') -Value $launcher.Replace('__AUDIO_FLAG__', '-noaudio') -Encoding Ascii
Set-Content -LiteralPath (Join-Path $packageDirectory 'launch-contra-headless.cmd') -Value $launcher.Replace('__AUDIO_FLAG__', '-noaudio').Replace(' -win ', ' -headless ') -Encoding Ascii
$pythonPath = Join-Path $repoDirectory 'tools\generals-mcp\.venv\Scripts\python.exe'
$clientConfiguration = @"
[mcp_servers.generals_zh]
command = '$pythonPath'
args = ["-m", "generals_mcp", "--pipe", "local", "--timeout", "5", "--control-mode", "realtime"]
tool_timeout_sec = 30
"@
$utf8NoBom = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText((Join-Path $packageDirectory 'codex-mcp.toml'), $clientConfiguration, $utf8NoBom)
$manifest = @{
    version = '0.3.0'
    engine = 'GeneralsX'
    executable = $packagedExecutable
    sha256 = (Get-FileHash -LiteralPath $packagedExecutable -Algorithm SHA256).Hash
    game_assets = $assetDirectory
    mcp_python = $pythonPath
    pipe = 'local'
    control_mode = 'realtime'
    app_local_graphics = [bool]$GraphicsRuntimeDirectory
    scope = 'structured snapshots, gameplay batches, bounded reactions, replay management, single-player sessions and LAN lobby control'
}
[System.IO.File]::WriteAllText((Join-Path $packageDirectory 'manifest.json'), ($manifest | ConvertTo-Json), $utf8NoBom)
Write-Output "Packaged executable: $packagedExecutable"
Write-Output "Launcher: $(Join-Path $packageDirectory 'launch-contra.cmd')"
Write-Output "Audio fallback: $(Join-Path $packageDirectory 'launch-contra-noaudio.cmd')"
Write-Output "MCP without a game window: $(Join-Path $packageDirectory 'launch-contra-headless.cmd')"
