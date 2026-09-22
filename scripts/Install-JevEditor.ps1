#Requires -Version 5.1
<#
.SYNOPSIS
Inspects or installs JevEditor source in one explicitly selected Unreal project.
.DESCRIPTION
Plan and UninstallPlan only inspect files and save reviewable JSON. Apply requires
an existing plan file and rechecks source/project/destination hashes. Close this
project's editor before applying. No credentials are loaded, no engine process
is controlled, and nothing is downloaded or built by this helper.
#>
[CmdletBinding()]
param(
    [ValidateSet('Inspect', 'Plan', 'UninstallPlan', 'Apply')]
    [string]$Action = 'Inspect',
    [string]$ProjectFile,
    [string]$SourcePlugin,
    [string]$EngineRoot,
    [ValidateRange(1024, 65535)][int]$Port = 9845,
    [string]$PlanFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$jevRepositoryRoot = Split-Path -Parent $PSScriptRoot
$null = Get-Command uv -ErrorAction Stop
if ([string]::IsNullOrWhiteSpace($SourcePlugin)) {
    $SourcePlugin = Join-Path $jevRepositoryRoot 'Plugins\JevEditor'
}

$jevArguments = @('--directory', $jevRepositoryRoot, 'run', '--frozen', 'jev-unreal', 'setup')
if ($Action -eq 'Apply') {
    if ([string]::IsNullOrWhiteSpace($PlanFile)) { throw 'Apply requires -PlanFile.' }
    $jevArguments += @('apply', $PlanFile)
}
else {
    if ([string]::IsNullOrWhiteSpace($ProjectFile)) { throw 'Select an explicit -ProjectFile .uproject.' }
    switch ($Action) {
        'Inspect' {
            $jevArguments += @('inspect', '--project', $ProjectFile, '--source-plugin', $SourcePlugin, '--port', "$Port")
            if (-not [string]::IsNullOrWhiteSpace($EngineRoot)) {
                $jevArguments += @('--engine-root', $EngineRoot)
            }
        }
        'Plan' {
            $jevArguments += @('plan', '--project', $ProjectFile, '--source-plugin', $SourcePlugin)
        }
        'UninstallPlan' { $jevArguments += @('uninstall-plan', '--project', $ProjectFile) }
    }
    if ($Action -ne 'Inspect' -and -not [string]::IsNullOrWhiteSpace($PlanFile)) {
        $jevArguments += @('--output', $PlanFile)
    }
}
& uv @jevArguments
if ($LASTEXITCODE -ne 0) { throw "Jev setup failed with exit code $LASTEXITCODE." }
