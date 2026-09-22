#Requires -Version 5.1
<#
.SYNOPSIS
Starts the stdio MCP server with local credentials in its process environment.
.DESCRIPTION
Respects an existing OPENROUTER_API_KEY; otherwise decrypts the current user's
%LOCALAPPDATA%/JevUnreal/openrouter.dpapi if available. JEV_BRIDGE_TOKEN_FILE may
point to a local file containing the editor bridge token. Never pass tokens as
command-line arguments. Set JEV_EXPECTED_PROJECT to the absolute .uproject path
of the intended Unreal project. Standard output is reserved for the MCP protocol.
#>
[CmdletBinding()]
param(
    [string]$BridgeTokenFile = $env:JEV_BRIDGE_TOKEN_FILE,
    [string]$ExpectedProject = $env:JEV_EXPECTED_PROJECT,
    [string]$CatalogFile = $env:JEV_CATALOG_FILE
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
$jevSecureKey = $null
$jevKeyPointer = [IntPtr]::Zero
$jevExitCode = 1

try {
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

    if (-not [string]::IsNullOrWhiteSpace($BridgeTokenFile)) {
        $jevResolvedTokenFile = (Resolve-Path -LiteralPath $BridgeTokenFile -ErrorAction Stop).ProviderPath
        if (-not (Test-Path -LiteralPath $jevResolvedTokenFile -PathType Leaf)) {
            throw 'JEV_BRIDGE_TOKEN_FILE must point to a local file.'
        }
        $jevBridgeToken = [IO.File]::ReadAllText($jevResolvedTokenFile).Trim()
        if ($jevBridgeToken.Length -lt 32 -or $jevBridgeToken -match '\s') {
            throw 'The bridge token must contain at least 32 characters and no whitespace.'
        }
        [Environment]::SetEnvironmentVariable('JEV_BRIDGE_TOKEN', $jevBridgeToken, 'Process')
        $jevBridgeToken = $null
    }

    if (-not [string]::IsNullOrWhiteSpace($ExpectedProject)) {
        [Environment]::SetEnvironmentVariable('JEV_EXPECTED_PROJECT', $ExpectedProject, 'Process')
    }
    if (-not [string]::IsNullOrWhiteSpace($CatalogFile)) {
        $jevResolvedCatalog = (Resolve-Path -LiteralPath $CatalogFile -ErrorAction Stop).ProviderPath
        if (-not (Test-Path -LiteralPath $jevResolvedCatalog -PathType Leaf)) {
            throw 'JEV_CATALOG_FILE must point to an explicit catalog JSON file.'
        }
        [Environment]::SetEnvironmentVariable('JEV_CATALOG_FILE', $jevResolvedCatalog, 'Process')
    }

    & uv --directory $jevRepositoryRoot run --frozen jev-unreal serve
    $jevExitCode = $LASTEXITCODE
}
finally {
    [Environment]::SetEnvironmentVariable('OPENROUTER_API_KEY', $jevPreviousApiKey, 'Process')
    [Environment]::SetEnvironmentVariable('JEV_BRIDGE_TOKEN', $jevPreviousBridgeToken, 'Process')
    [Environment]::SetEnvironmentVariable('JEV_EXPECTED_PROJECT', $jevPreviousExpectedProject, 'Process')
    [Environment]::SetEnvironmentVariable('JEV_CATALOG_FILE', $jevPreviousCatalogFile, 'Process')
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
