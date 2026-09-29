[CmdletBinding()]
param([string]$EngineRoot = 'C:\Program Files\UE_5.8')
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$pluginRoot = Join-Path $repositoryRoot 'Plugins\JevEditor'
$projectFile = Join-Path $repositoryRoot 'examples\JevSandbox\JevSandbox.uproject'
$editorCommandlet = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if (-not (Test-Path -LiteralPath $editorCommandlet -PathType Leaf)) { throw 'Licensed UnrealEditor-Cmd.exe was not found.' }
if (-not (Test-Path -LiteralPath $projectFile -PathType Leaf)) { throw 'The isolated JevSandbox project was not found.' }
$outputDirectory = Join-Path $repositoryRoot 'artifacts\localization'
$contentDirectory = Join-Path $pluginRoot 'Content\Localization\JevEditor'
$null = New-Item -ItemType Directory -Path $outputDirectory -Force
foreach ($culture in @('fr', 'es')) {
    $cultureDirectory = Join-Path $outputDirectory $culture
    $null = New-Item -ItemType Directory -Path $cultureDirectory -Force
    Copy-Item -LiteralPath (Join-Path $pluginRoot "Localization\JevEditor\$culture\JevEditor.po") -Destination (Join-Path $cultureDirectory 'JevEditor.po') -Force
}
$template = Get-Content -LiteralPath (Join-Path $pluginRoot 'Config\Localization\JevEditor.ini.template') -Raw
$configuration = $template.Replace('@OUTPUT@', $outputDirectory.Replace('\', '/')).Replace('@SOURCE@', (Join-Path $pluginRoot 'Source\JevEditor').Replace('\', '/'))
$configPath = Join-Path $outputDirectory 'JevEditor.ini'
[IO.File]::WriteAllText($configPath, $configuration, [Text.UTF8Encoding]::new($false))
$stdoutPath = Join-Path $outputDirectory 'gather.stdout.log'
$stderrPath = Join-Path $outputDirectory 'gather.stderr.log'
$arguments = @(('"' + $projectFile + '"'), '-run=GatherText', ('-config="' + $configPath + '"'), '-SkipNestedMacroPrepass', '-unattended', '-nop4', '-NullRHI', '-nosplash')
$process = Start-Process -FilePath $editorCommandlet -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
if (-not $process.WaitForExit(300000)) {
    Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
    throw 'Isolated localization compilation exceeded five minutes. Inspect the retained logs.'
}
if ($process.ExitCode -ne 0) { throw "Localization commandlet failed with exit code $($process.ExitCode). Inspect $stdoutPath and $stderrPath." }
$null = New-Item -ItemType Directory -Path $contentDirectory -Force
foreach ($culture in @('en', 'fr', 'es')) {
    $resource = Join-Path $outputDirectory "$culture\JevEditor.locres"
    if (-not (Test-Path -LiteralPath $resource -PathType Leaf) -or (Get-Item -LiteralPath $resource).Length -eq 0) { throw "Missing compiled localization resource for $culture." }
    $destination = Join-Path $contentDirectory $culture
    $null = New-Item -ItemType Directory -Path $destination -Force
    Copy-Item -LiteralPath $resource -Destination (Join-Path $destination 'JevEditor.locres') -Force
}
$metadata = Join-Path $outputDirectory 'JevEditor.locmeta'
if (-not (Test-Path -LiteralPath $metadata -PathType Leaf)) { throw 'Missing compiled localization metadata.' }
Copy-Item -LiteralPath $metadata -Destination (Join-Path $contentDirectory 'JevEditor.locmeta') -Force
Write-Output 'Compiled English, French and Spanish Jev resources. Restart the isolated editor before localization acceptance. Translation status: draft; human review remains open.'
