[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\Program Files\UE_5.8',
    [switch]$Headless,
    [switch]$AutomationTests,
    [switch]$RenderedReviewTest,
    [switch]$Unattended,
    [ValidateRange(1024, 65535)][int]$Port = 9845
)
$ErrorActionPreference = 'Stop'
if ($RenderedReviewTest -and ($Headless -or $AutomationTests)) { throw 'RenderedReviewTest requires a rendered editor; run it separately from headless automation.' }
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
elseif ($Unattended -and -not $RenderedReviewTest) { $editorArguments += @('-Unattended', '-NoPause') }
if ($AutomationTests) {
    $reportPath = Join-Path $repositoryRoot 'artifacts\unreal-automation'
    $editorArguments += @('-ExecCmds="Automation RunTests Jev.Editor"', '-TestExit="Automation Test Queue Empty"', ('-ReportExportPath="' + $reportPath + '"'))
}
if ($RenderedReviewTest) {
    $reportPath = Join-Path $repositoryRoot 'artifacts\unreal-rendered'
    $editorArguments += @('-Unattended', '-NoPause', '-ExecCmds="Automation RunTests Jev.Rendered"', '-TestExit="Automation Test Queue Empty"', ('-ReportExportPath="' + $reportPath + '"'))
}
$jevPreviousPort = $env:JEV_BRIDGE_PORT
$env:JEV_BRIDGE_PORT = [string]$Port
$startedAt = Get-Date
try { $process = Start-Process -FilePath $editorFile -ArgumentList $editorArguments -PassThru -WindowStyle Hidden }
finally { $env:JEV_BRIDGE_PORT = $jevPreviousPort }
Write-Output "Started isolated JevSandbox editor (PID $($process.Id))."
if ($AutomationTests -or $RenderedReviewTest) {
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
    if ($RenderedReviewTest) {
        if ([int]$report.failed -ne 0 -or [int]$report.notRun -ne 0 -or [int]$report.inProcess -ne 0 -or $passedCount -ne 2) { throw 'Rendered review automation failed or incomplete.' }
        foreach ($expectedTest in @('Jev.Rendered.ReviewPanel', 'Jev.Rendered.ReviewWorkflow')) {
            $testResult = @($report.tests | Where-Object { $_.fullTestPath -eq $expectedTest })
            if ($testResult.Count -ne 1 -or $testResult[0].state -ne 'Success') { throw "Expected rendered automation test did not pass: $expectedTest" }
        }
        Write-Output 'Rendered review automation passed. Inspect Saved/Automation/Jev/Review*.png for visual acceptance.'
        return
    }
    if ([int]$report.failed -ne 0 -or [int]$report.notRun -ne 0 -or [int]$report.inProcess -ne 0 -or $passedCount -lt 27) { throw "Unreal automation failed or incomplete: passed=$passedCount, failed=$($report.failed)." }
    foreach ($expectedTest in @('Jev.Editor.PlanLifecycle', 'Jev.Editor.PlanSafety', 'Jev.Editor.SchemaSafety', 'Jev.Editor.ContextInspection', 'Jev.Editor.SceneValidation', 'Jev.Editor.AssetInspection', 'Jev.Editor.CaptureSafety', 'Jev.Editor.FrameSafety', 'Jev.Editor.StaticMeshPlacement', 'Jev.Editor.ActorDetails', 'Jev.Editor.ExpectedState', 'Jev.Editor.MetadataEdits', 'Jev.Editor.MaterialEdits', 'Jev.Editor.EditRollback', 'Jev.Editor.NativePlanHistory', 'Jev.Editor.ReviewSelection', 'Jev.Editor.BlueprintInspection', 'Jev.Editor.AssetProjectInspection', 'Jev.Editor.ValidationJobs', 'Jev.Editor.ValidationJobSafety', 'Jev.Editor.FunctionalJobs', 'Jev.Editor.MeshReplacement', 'Jev.Editor.MeshDuplicate', 'Jev.Editor.MeshGuards', 'Jev.Editor.MeshRollback', 'Jev.Editor.ReviewPresentation', 'Jev.Editor.ReviewRecovery')) {
        $testResult = @($report.tests | Where-Object { $_.fullTestPath -eq $expectedTest })
        if ($testResult.Count -ne 1 -or $testResult[0].state -ne 'Success') { throw "Expected Unreal automation test did not pass: $expectedTest" }
    }
    Write-Output "Unreal automation verified: $passedCount passed, $($report.failed) failed. Report: artifacts\unreal-automation\index.json"
}
