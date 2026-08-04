[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
param(
    [string]$ToolDirectory = (Join-Path $env:LOCALAPPDATA 'skillcli'),
    [switch]$NoPathUpdate
)

$ErrorActionPreference = 'Stop'
$Repository = 'WillEastbury/skillcli'
$Ref = 'main'

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw 'GitHub CLI (gh) is required and must be authenticated to the managed repository.'
}
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    throw 'Python 3 is required to run the managed Skill Zero CLI.'
}
$commit = (& gh api "repos/$Repository/commits/$Ref" --jq '.sha' 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $commit) {
    throw "Cannot access the public CLI repository $Repository."
}

$cliEndpoint = "repos/$Repository/contents/skills/skill-zero/skillcli.py?ref=$commit"
$sourcesEndpoint = "repos/$Repository/contents/skill-sources.json?ref=$commit"
$cliPath = Join-Path $ToolDirectory 'skillcli.py'
$wrapperPath = Join-Path $ToolDirectory 'skillcli.cmd'
$sourcesPath = Join-Path $ToolDirectory 'sources.json'

if (-not $PSCmdlet.ShouldProcess(
    'detected Copilot CLI, Scout, and Co-Work folders',
    "Install skillcli and Skill Zero from the public catalogue"
)) {
    return
}

New-Item -ItemType Directory -Force -Path $ToolDirectory | Out-Null
$cliContent = & gh api -H 'Accept: application/vnd.github.raw+json' $cliEndpoint
if ($LASTEXITCODE -ne 0 -or -not $cliContent) {
    throw 'Failed to download the managed Skill Zero CLI.'
}
[IO.File]::WriteAllText(
    $cliPath,
    ($cliContent -join "`n") + "`n",
    [Text.UTF8Encoding]::new($false)
)
[IO.File]::WriteAllText(
    $wrapperPath,
    "@echo off`r`npython `"%~dp0skillcli.py`" %*`r`n",
    [Text.ASCIIEncoding]::new()
)
$sourcesContent = & gh api -H 'Accept: application/vnd.github.raw+json' $sourcesEndpoint
if ($LASTEXITCODE -ne 0 -or -not $sourcesContent) {
    throw 'Failed to download the managed catalogue source configuration.'
}
[IO.File]::WriteAllText(
    $sourcesPath,
    ($sourcesContent -join "`n") + "`n",
    [Text.UTF8Encoding]::new($false)
)

if (-not $NoPathUpdate) {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $pathParts = @($userPath -split ';' | Where-Object { $_ })
    if ($pathParts -notcontains $ToolDirectory) {
        $newPath = (@($pathParts) + $ToolDirectory) -join ';'
        [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
    }
}
$env:Path = "$ToolDirectory;$env:Path"

& $wrapperPath install --skill skill-zero
if ($LASTEXITCODE -ne 0) {
    throw 'Skill Zero installation failed.'
}

Write-Output ""
Write-Output "Command installed: skillcli"
Write-Output "Public CLI commit: $commit"
Write-Output 'Try: skillcli search --role seller --query "prompt quality"'
