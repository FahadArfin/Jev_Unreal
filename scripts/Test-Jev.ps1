#Requires -Version 5.1
[CmdletBinding()]
param([switch]$SmokeOnly, [switch]$WorkflowSmoke)
$ErrorActionPreference = 'Stop'
if ($SmokeOnly -and $WorkflowSmoke) { throw 'Choose SmokeOnly or WorkflowSmoke, not both.' }
. "$PSScriptRoot/Import-JevSecurity.ps1"
$jevRoot = Split-Path -Parent $PSScriptRoot
$jevPriorKey = $env:OPENROUTER_API_KEY
$jevSecureKey = $null
$jevPointer = [IntPtr]::Zero
try {
    if ([string]::IsNullOrWhiteSpace($env:OPENROUTER_API_KEY)) {
        $jevKeyFile = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'JevUnreal\openrouter.dpapi'
        if (-not (Test-Path -LiteralPath $jevKeyFile)) { throw 'Run scripts/Set-OpenRouterKey.ps1 first.' }
        $jevSecureKey = ConvertTo-SecureString ([IO.File]::ReadAllText($jevKeyFile).Trim())
        $jevPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($jevSecureKey)
        $env:OPENROUTER_API_KEY = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($jevPointer)
    }
    if ($WorkflowSmoke) {
        & uv --directory $jevRoot run --frozen python (Join-Path $PSScriptRoot 'smoke_semantics.py') --include-route --output (Join-Path $jevRoot 'artifacts\semantic-smoke.json')
    } elseif ($SmokeOnly) {
        & uv --directory $jevRoot run --frozen jev-unreal smoke
    } else {
        & uv --directory $jevRoot run --frozen python (Join-Path $PSScriptRoot 'evaluate.py') --output (Join-Path $jevRoot 'artifacts\jev-evaluation.json')
    }
    if ($LASTEXITCODE -ne 0) { throw 'Live Jev validation failed; inspect the sanitized output above.' }
} finally {
    $env:OPENROUTER_API_KEY = $jevPriorKey
    if ($jevPointer -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($jevPointer) }
    if ($null -ne $jevSecureKey) { $jevSecureKey.Dispose() }
    $jevPriorKey = $null
}
