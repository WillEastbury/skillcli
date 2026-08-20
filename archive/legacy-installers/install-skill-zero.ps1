[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
param(
    [string]$ToolDirectory = (Join-Path $env:LOCALAPPDATA 'skillcli'),
    [switch]$NoPathUpdate
)

if ($NoPathUpdate) {
    $env:SKILLCLI_NO_PATH_UPDATE = '1'
}
$env:SKILLCLI_TOOL_DIRECTORY = $ToolDirectory
& (Join-Path $PSScriptRoot 'install.ps1')
