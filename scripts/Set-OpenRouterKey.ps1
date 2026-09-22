#Requires -Version 5.1
<#
.SYNOPSIS
Stores an OpenRouter API key with Windows user-scoped DPAPI encryption.
.DESCRIPTION
Run interactively as the same Windows user who will run the MCP server.
The key is read with a hidden prompt and is never printed or saved in the repository.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Import-JevSecurity.ps1"

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'This helper requires Windows. On other platforms, set OPENROUTER_API_KEY in the server process environment.'
}

$jevLocalData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
if ([string]::IsNullOrWhiteSpace($jevLocalData)) {
    throw 'The current Windows user has no LocalApplicationData directory.'
}
$jevCredentialDirectory = Join-Path $jevLocalData 'JevUnreal'
$jevCredentialPath = Join-Path $jevCredentialDirectory 'openrouter.dpapi'
$jevSecureKey = $null
$jevEncryptedKey = $null

try {
    $jevSecureKey = Read-Host -Prompt 'Paste your OpenRouter API key (hidden)' -AsSecureString
    if ($jevSecureKey.Length -eq 0) {
        throw 'The key was empty. No credential was saved.'
    }

    $null = New-Item -ItemType Directory -Path $jevCredentialDirectory -Force
    $jevEncryptedKey = ConvertFrom-SecureString -SecureString $jevSecureKey
    [IO.File]::WriteAllText($jevCredentialPath, $jevEncryptedKey, [Text.UTF8Encoding]::new($false))
    Write-Host 'Saved the encrypted OpenRouter key for the current Windows user.'
    Write-Host 'Start the MCP server with scripts/Start-Mcp.ps1. Keep the encrypted file local.'
}
finally {
    if ($null -ne $jevSecureKey) {
        $jevSecureKey.Dispose()
    }
    $jevEncryptedKey = $null
}
