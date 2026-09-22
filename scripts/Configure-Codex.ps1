#Requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$jevRoot = Split-Path -Parent $PSScriptRoot
$jevConfigDir = Join-Path $jevRoot '.codex'
$jevConfigFile = Join-Path $jevConfigDir 'config.toml'
if (Test-Path -LiteralPath $jevConfigFile) {
    throw 'Project .codex/config.toml already exists. Merge the documented MCP entry manually; it has not been overwritten.'
}
$jevTokenFile = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'JevUnreal\bridge.token'
if (-not (Test-Path -LiteralPath $jevTokenFile)) { throw 'Run scripts/Initialize-Local.ps1 first.' }
$jevLauncher = (Join-Path $PSScriptRoot 'Start-Mcp.ps1').Replace('\', '/')
$jevTokenPath = $jevTokenFile.Replace('\', '/')
$jevProject = (Join-Path $jevRoot 'examples\JevSandbox\JevSandbox.uproject').Replace('\', '/')
foreach ($jevPath in @($jevLauncher, $jevTokenPath, $jevProject)) {
    if ($jevPath.Contains('"') -or $jevPath.Contains("`n")) { throw 'Unsupported quote/newline in path.' }
}
$jevConfig = @"
[mcp_servers.jev_unreal]
command = "powershell.exe"
args = ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "$jevLauncher", "-BridgeTokenFile", "$jevTokenPath", "-ExpectedProject", "$jevProject"]
startup_timeout_sec = 30
tool_timeout_sec = 60
"@
$null = New-Item -ItemType Directory -Path $jevConfigDir -Force
[IO.File]::WriteAllText($jevConfigFile, $jevConfig, [Text.UTF8Encoding]::new($false))
Write-Host 'Created local MCP configuration without secrets. Restart the MCP connection in Codex.'
