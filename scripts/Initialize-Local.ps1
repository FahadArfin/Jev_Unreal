#Requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$jevLocalDirectory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'JevUnreal'
$null = New-Item -ItemType Directory -Path $jevLocalDirectory -Force
$jevTokenFile = Join-Path $jevLocalDirectory 'bridge.token'
if (-not (Test-Path -LiteralPath $jevTokenFile)) {
    $jevBytes = New-Object byte[] 32
    $jevRandom = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $jevRandom.GetBytes($jevBytes) } finally { $jevRandom.Dispose() }
    [IO.File]::WriteAllText($jevTokenFile, [Convert]::ToBase64String($jevBytes), [Text.UTF8Encoding]::new($false))
}
$env:JEV_BRIDGE_TOKEN_FILE = $jevTokenFile
$env:JEV_BRIDGE_TOKEN = [IO.File]::ReadAllText($jevTokenFile).Trim()
$env:JEV_EXPECTED_PROJECT = Join-Path (Split-Path -Parent $PSScriptRoot) 'examples\JevSandbox\JevSandbox.uproject'
Write-Host 'Local bridge token ready. This PowerShell session now targets the isolated JevSandbox project.'
Write-Host 'Run scripts/Build-Unreal.ps1, then scripts/Launch-Unreal.ps1 in this session.'
