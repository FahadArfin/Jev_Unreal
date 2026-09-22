[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\Program Files\UE_5.8',
    [switch]$Headless,
    [switch]$AutomationTests
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$projectFile = Join-Path $repositoryRoot 'examples\JevSandbox\JevSandbox.uproject'
$editorFile = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
if ($Headless -or $AutomationTests) { $editorFile = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe' }
if (-not (Test-Path -LiteralPath $editorFile -PathType Leaf)) { throw "UnrealEditor.exe not found beneath $EngineRoot" }
if (-not $AutomationTests -and ($env:JEV_BRIDGE_TOKEN.Length -lt 32 -or $env:JEV_BRIDGE_TOKEN.Length -gt 256 -or $env:JEV_BRIDGE_TOKEN -notmatch '^[\x21-\x7E]+$')) {
    throw 'Set JEV_BRIDGE_TOKEN to a random 32-256 character printable ASCII secret before launching. Never pass the token as a command-line argument.'
}
$editorArguments = @(('"' + $projectFile + '"'), '-NoSplash', '-NoSound', '-NoLiveCoding', '-NoSourceControl')
if ($Headless -or $AutomationTests) { $editorArguments += @('-NullRHI', '-Unattended', '-NoPause') }
if ($AutomationTests) {
    $reportPath = Join-Path $repositoryRoot 'artifacts\unreal-automation'
    $editorArguments += @('-ExecCmds="Automation RunTests Jev.Editor"', '-TestExit="Automation Test Queue Empty"', ('-ReportExportPath="' + $reportPath + '"'))
}
$startedAt = Get-Date
$process = Start-Process -FilePath $editorFile -ArgumentList $editorArguments -PassThru -WindowStyle Hidden
Write-Output "Started isolated JevSandbox editor (PID $($process.Id))."
if ($AutomationTests) {
    if (-not $process.WaitForExit(600000)) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        throw 'Isolated Unreal automation exceeded 10 minutes. Inspect the sandbox log.'
    }
    if ($process.ExitCode -ne 0) { throw "Unreal automation process exited with code $($process.ExitCode). Inspect artifacts\unreal-automation and JevSandbox Saved\Logs." }
    $reportFile = Join-Path $reportPath 'index.json'
    if (-not (Test-Path -LiteralPath $reportFile -PathType Leaf)) { throw 'Unreal exited without producing an automation report.' }
    if ((Get-Item -LiteralPath $reportFile).LastWriteTime -lt $startedAt) { throw 'The automation report is stale.' }
    $report = Get-Content -LiteralPath $reportFile -Raw | ConvertFrom-Json
    $passedCount = [int]$report.succeeded + [int]$report.succeededWithWarnings
    if ([int]$report.failed -ne 0 -or [int]$report.notRun -ne 0 -or [int]$report.inProcess -ne 0 -or $passedCount -lt 14) { throw "Unreal automation failed or incomplete: passed=$passedCount, failed=$($report.failed)." }
    foreach ($expectedTest in @('Jev.Editor.PlanLifecycle', 'Jev.Editor.PlanSafety', 'Jev.Editor.SchemaSafety', 'Jev.Editor.ContextInspection', 'Jev.Editor.SceneValidation', 'Jev.Editor.AssetInspection', 'Jev.Editor.CaptureSafety', 'Jev.Editor.FrameSafety', 'Jev.Editor.StaticMeshPlacement', 'Jev.Editor.ActorDetails', 'Jev.Editor.ExpectedState', 'Jev.Editor.MetadataEdits', 'Jev.Editor.MaterialEdits', 'Jev.Editor.EditRollback')) {
        $testResult = @($report.tests | Where-Object { $_.fullTestPath -eq $expectedTest })
        if ($testResult.Count -ne 1 -or $testResult[0].state -ne 'Success') { throw "Expected Unreal automation test did not pass: $expectedTest" }
    }
    Write-Output "Unreal automation verified: $passedCount passed, $($report.failed) failed. Report: artifacts\unreal-automation\index.json"
}
