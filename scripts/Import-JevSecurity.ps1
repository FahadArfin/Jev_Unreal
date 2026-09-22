#Requires -Version 5.1
# Dot-source this helper. Select the module shipped with the running PowerShell
# instead of searching PSModulePath, which may contain another edition's modules.
# Do not use -Force: the built-in module may already be loaded in this session.
$jevSecurityModule = [IO.Path]::Combine(
    $PSHOME, 'Modules', 'Microsoft.PowerShell.Security', 'Microsoft.PowerShell.Security.psd1'
)
Import-Module -Name $jevSecurityModule -ErrorAction Stop
