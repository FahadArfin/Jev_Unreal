[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\Program Files\UE_5.8',
    [ValidateSet('Development', 'DebugGame')][string]$Configuration = 'Development'
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$projectFile = Join-Path $repositoryRoot 'examples\JevSandbox\JevSandbox.uproject'
$buildTool = Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat'
if (-not (Test-Path -LiteralPath $buildTool -PathType Leaf)) { throw "Unreal Build.bat not found beneath $EngineRoot" }
if (-not (Test-Path -LiteralPath $projectFile -PathType Leaf)) { throw "Sandbox project not found: $projectFile" }
& $buildTool JevSandboxEditor Win64 $Configuration "-Project=$projectFile" -WaitMutex -NoHotReloadFromIDE
if ($LASTEXITCODE -ne 0) { throw "Unreal build failed with exit code $LASTEXITCODE" }
