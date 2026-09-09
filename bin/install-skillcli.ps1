[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateLength(1, 511)]
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$')]
    [string]$Source
)

$ErrorActionPreference = 'Stop'
$uri = 'https://raw.githubusercontent.com/WillEastbury/skillcli/3c9847e14e9c03c37f5182a54fd52490c45e3df7/bin/skillcli.exe'
$expected = 'FE8BB447D313A47ECBC4932C84D1058D208D190568ACE7ADF42C9D25A7DFD6BA'
$directory = Join-Path $env:LOCALAPPDATA 'SkillCLI\Bootstrap'
$installer = Join-Path $directory 'skillcli.exe'

try {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    Invoke-WebRequest $uri -OutFile $installer
    if ((Get-FileHash $installer -Algorithm SHA256).Hash -ne $expected) {
        throw 'SkillCLI checksum verification failed.'
    }
    Unblock-File -LiteralPath $installer
    & $installer setup --source $Source
    if ($LASTEXITCODE) {
        throw "SkillCLI setup failed with exit code $LASTEXITCODE."
    }
} finally {
    Remove-Item $installer -Force -ErrorAction SilentlyContinue
}
