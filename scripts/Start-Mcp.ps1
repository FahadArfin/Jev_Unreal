#Requires -Version 5.1
<#
.SYNOPSIS
Starts the stdio MCP server with local credentials in its process environment.
.DESCRIPTION
Respects an existing OPENROUTER_API_KEY; otherwise decrypts the current user's
%LOCALAPPDATA%/JevUnreal/openrouter.dpapi if available. JEV_BRIDGE_TOKEN_FILE may
point to a local file containing the editor bridge token. Never pass tokens as
command-line arguments. Set JEV_EXPECTED_PROJECT to the absolute .uproject path
of the intended Unreal project. Alternatively select -ProfilesFile and -Profile;
the Python server then reads only that profile's bounded token file. A profile
atomically selects its project, URL and token file. Standard output is reserved
for the MCP protocol.
#>
[CmdletBinding()]
param(
    [string]$BridgeTokenFile = $env:JEV_BRIDGE_TOKEN_FILE,
    [string]$ExpectedProject = $env:JEV_EXPECTED_PROJECT,
    [string]$CatalogFile = $env:JEV_CATALOG_FILE,
    [string]$BridgeUrl = $env:JEV_BRIDGE_URL,
    [string]$ProfilesFile = $env:JEV_PROFILES_FILE,
    [string]$Profile = $env:JEV_PROFILE
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Import-JevSecurity.ps1"

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'This helper requires Windows. On other platforms, configure environment variables and run uv run --frozen jev-unreal serve.'
}

$null = Get-Command uv -ErrorAction Stop
$jevRepositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $jevRepositoryRoot 'pyproject.toml') -PathType Leaf)) {
    throw 'Cannot find pyproject.toml in the parent of this scripts directory.'
}

$jevPreviousApiKey = [Environment]::GetEnvironmentVariable('OPENROUTER_API_KEY', 'Process')
$jevPreviousBridgeToken = [Environment]::GetEnvironmentVariable('JEV_BRIDGE_TOKEN', 'Process')
$jevPreviousExpectedProject = [Environment]::GetEnvironmentVariable('JEV_EXPECTED_PROJECT', 'Process')
$jevPreviousCatalogFile = [Environment]::GetEnvironmentVariable('JEV_CATALOG_FILE', 'Process')
$jevPreviousBridgeTokenFile = [Environment]::GetEnvironmentVariable('JEV_BRIDGE_TOKEN_FILE', 'Process')
$jevPreviousBridgeUrl = [Environment]::GetEnvironmentVariable('JEV_BRIDGE_URL', 'Process')
$jevPreviousBridgePort = [Environment]::GetEnvironmentVariable('JEV_BRIDGE_PORT', 'Process')
$jevPreviousProfilesFile = [Environment]::GetEnvironmentVariable('JEV_PROFILES_FILE', 'Process')
$jevPreviousProfile = [Environment]::GetEnvironmentVariable('JEV_PROFILE', 'Process')
$jevSecureKey = $null
$jevKeyPointer = [IntPtr]::Zero
$jevExitCode = 1

try {
    $jevUsesProfile = -not [string]::IsNullOrWhiteSpace($ProfilesFile)
    if ($jevUsesProfile -ne (-not [string]::IsNullOrWhiteSpace($Profile))) {
        throw 'Set both -ProfilesFile and -Profile together.'
    }
    if ($jevUsesProfile) {
        foreach ($jevLegacyArgument in @('BridgeTokenFile', 'ExpectedProject', 'BridgeUrl')) {
            if ($PSBoundParameters.ContainsKey($jevLegacyArgument)) {
                throw 'A profile cannot be combined with explicit legacy project, URL or token-file arguments.'
            }
        }
        [Environment]::SetEnvironmentVariable('JEV_PROFILES_FILE', [IO.Path]::GetFullPath($ProfilesFile), 'Process')
        [Environment]::SetEnvironmentVariable('JEV_PROFILE', $Profile, 'Process')
        foreach ($jevLegacyVariable in @('JEV_BRIDGE_TOKEN', 'JEV_BRIDGE_TOKEN_FILE', 'JEV_EXPECTED_PROJECT', 'JEV_BRIDGE_URL', 'JEV_BRIDGE_PORT')) {
            [Environment]::SetEnvironmentVariable($jevLegacyVariable, $null, 'Process')
        }
    }
    else {
        [Environment]::SetEnvironmentVariable('JEV_PROFILES_FILE', $null, 'Process')
        [Environment]::SetEnvironmentVariable('JEV_PROFILE', $null, 'Process')
        if (-not [string]::IsNullOrWhiteSpace($BridgeTokenFile)) {
            [Environment]::SetEnvironmentVariable('JEV_BRIDGE_TOKEN_FILE', [IO.Path]::GetFullPath($BridgeTokenFile), 'Process')
            # The Python settings loader validates a regular local file and reads at most 4 KiB.
            [Environment]::SetEnvironmentVariable('JEV_BRIDGE_TOKEN', $null, 'Process')
        }
        if (-not [string]::IsNullOrWhiteSpace($ExpectedProject)) {
            [Environment]::SetEnvironmentVariable('JEV_EXPECTED_PROJECT', $ExpectedProject, 'Process')
        }
        if (-not [string]::IsNullOrWhiteSpace($BridgeUrl)) {
            [Environment]::SetEnvironmentVariable('JEV_BRIDGE_URL', $BridgeUrl, 'Process')
        }
    }
    if ([string]::IsNullOrWhiteSpace($jevPreviousApiKey)) {
        $jevLocalData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
        $jevCredentialPath = Join-Path (Join-Path $jevLocalData 'JevUnreal') 'openrouter.dpapi'
        if (Test-Path -LiteralPath $jevCredentialPath -PathType Leaf) {
            $jevSecureKey = ConvertTo-SecureString -String ([IO.File]::ReadAllText($jevCredentialPath).Trim())
            $jevKeyPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($jevSecureKey)
            [Environment]::SetEnvironmentVariable(
                'OPENROUTER_API_KEY',
                [Runtime.InteropServices.Marshal]::PtrToStringBSTR($jevKeyPointer),
                'Process'
            )
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($CatalogFile)) {
        $jevResolvedCatalog = (Resolve-Path -LiteralPath $CatalogFile -ErrorAction Stop).ProviderPath
        if (-not (Test-Path -LiteralPath $jevResolvedCatalog -PathType Leaf)) {
            throw 'JEV_CATALOG_FILE must point to an explicit catalog JSON file.'
        }
        [Environment]::SetEnvironmentVariable('JEV_CATALOG_FILE', $jevResolvedCatalog, 'Process')
    }

    # Avoid keeping the generated console entry-point executable locked during upgrades.
    & uv --directory $jevRepositoryRoot run --frozen python -m jev_unreal serve
    $jevExitCode = $LASTEXITCODE
}
finally {
    [Environment]::SetEnvironmentVariable('OPENROUTER_API_KEY', $jevPreviousApiKey, 'Process')
    [Environment]::SetEnvironmentVariable('JEV_BRIDGE_TOKEN', $jevPreviousBridgeToken, 'Process')
    [Environment]::SetEnvironmentVariable('JEV_EXPECTED_PROJECT', $jevPreviousExpectedProject, 'Process')
    [Environment]::SetEnvironmentVariable('JEV_CATALOG_FILE', $jevPreviousCatalogFile, 'Process')
    [Environment]::SetEnvironmentVariable('JEV_BRIDGE_TOKEN_FILE', $jevPreviousBridgeTokenFile, 'Process')
    [Environment]::SetEnvironmentVariable('JEV_BRIDGE_URL', $jevPreviousBridgeUrl, 'Process')
    [Environment]::SetEnvironmentVariable('JEV_BRIDGE_PORT', $jevPreviousBridgePort, 'Process')
    [Environment]::SetEnvironmentVariable('JEV_PROFILES_FILE', $jevPreviousProfilesFile, 'Process')
    [Environment]::SetEnvironmentVariable('JEV_PROFILE', $jevPreviousProfile, 'Process')
    if ($jevKeyPointer -ne [IntPtr]::Zero) {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($jevKeyPointer)
    }
    if ($null -ne $jevSecureKey) {
        $jevSecureKey.Dispose()
    }
    $jevPreviousApiKey = $null
    $jevPreviousBridgeToken = $null
}

exit $jevExitCode
