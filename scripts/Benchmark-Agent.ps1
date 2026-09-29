#Requires -Version 5.1
<#
.SYNOPSIS
Runs bounded actual-agent trials using the selected project and a local OpenRouter credential.
.DESCRIPTION
Requires an installed, authenticated Codex CLI and an explicitly bound editor/profile.
OffOnly makes one real Codex trial and zero Jev requests. Default paired mode uses
the saved DPAPI credential, falling back to an existing process key only when no
saved credential exists. Keys never appear in arguments or output. Reports contain
sanitized measurements, not transcripts. Use a new ignored/private output directory.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$ProfilesFile = $env:JEV_PROFILES_FILE,
    [string]$Profile = $env:JEV_PROFILE,
    [string]$CodexExecutable,
    [ValidateRange(1, 3)][int]$Repetitions = 1,
    [switch]$OffOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Import-JevSecurity.ps1"
$jevPrevious = @{}
foreach ($jevName in @('OPENROUTER_API_KEY', 'JEV_CREDENTIAL_SOURCE', 'JEV_PROFILES_FILE', 'JEV_PROFILE', 'JEV_BRIDGE_TOKEN', 'JEV_BRIDGE_TOKEN_FILE', 'JEV_EXPECTED_PROJECT', 'JEV_BRIDGE_URL', 'JEV_BRIDGE_PORT')) {
    $jevPrevious[$jevName] = [Environment]::GetEnvironmentVariable($jevName, 'Process')
}
$jevSecure = $null
$jevPointer = [IntPtr]::Zero
$jevExitCode = 1
try {
    if ([string]::IsNullOrWhiteSpace($ProfilesFile) -ne [string]::IsNullOrWhiteSpace($Profile)) { throw 'Select both ProfilesFile and Profile together.' }
    if (-not [string]::IsNullOrWhiteSpace($ProfilesFile)) {
        [Environment]::SetEnvironmentVariable('JEV_PROFILES_FILE', [IO.Path]::GetFullPath($ProfilesFile), 'Process')
        [Environment]::SetEnvironmentVariable('JEV_PROFILE', $Profile, 'Process')
        foreach ($jevName in @('JEV_BRIDGE_TOKEN', 'JEV_BRIDGE_TOKEN_FILE', 'JEV_EXPECTED_PROJECT', 'JEV_BRIDGE_URL', 'JEV_BRIDGE_PORT')) {
            [Environment]::SetEnvironmentVariable($jevName, $null, 'Process')
        }
    }
    if ($OffOnly) {
        # No routing call is made in off-only mode; don't decrypt an unused key.
        [Environment]::SetEnvironmentVariable('OPENROUTER_API_KEY', $null, 'Process')
    } else {
        $jevCredential = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'JevUnreal\openrouter.dpapi'
        if (Test-Path -LiteralPath $jevCredential -PathType Leaf) {
            $jevSecure = ConvertTo-SecureString -String ([IO.File]::ReadAllText($jevCredential).Trim())
            $jevPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($jevSecure)
            [Environment]::SetEnvironmentVariable('OPENROUTER_API_KEY', [Runtime.InteropServices.Marshal]::PtrToStringBSTR($jevPointer), 'Process')
            [Environment]::SetEnvironmentVariable('JEV_CREDENTIAL_SOURCE', 'saved_dpapi', 'Process')
        } elseif ([string]::IsNullOrWhiteSpace($env:OPENROUTER_API_KEY)) {
            throw 'Save an OpenRouter key with Set-OpenRouterKey.ps1 before paired trials.'
        } else {
            [Environment]::SetEnvironmentVariable('JEV_CREDENTIAL_SOURCE', 'process_environment', 'Process')
        }
    }
    $jevArguments = @('-m', 'jev_unreal.codex_benchmark', '--output', [IO.Path]::GetFullPath($OutputDirectory), '--repetitions', $Repetitions.ToString())
    if ($OffOnly) { $jevArguments += '--off-only' }
    if (-not [string]::IsNullOrWhiteSpace($CodexExecutable)) { $jevArguments += @('--codex', $CodexExecutable) }
    & uv --directory (Split-Path -Parent $PSScriptRoot) run --frozen python @jevArguments
    $jevExitCode = $LASTEXITCODE
} finally {
    foreach ($jevName in $jevPrevious.Keys) { [Environment]::SetEnvironmentVariable($jevName, $jevPrevious[$jevName], 'Process') }
    if ($jevPointer -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($jevPointer) }
    if ($null -ne $jevSecure) { $jevSecure.Dispose() }
    $jevPrevious.Clear()
}
exit $jevExitCode
