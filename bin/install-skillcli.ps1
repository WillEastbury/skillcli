[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateLength(1, 511)]
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$')]
    [string]$Source
)

$ErrorActionPreference = 'Stop'
$uri = 'https://raw.githubusercontent.com/WillEastbury/skillcli/c167a27284db7cd624e5ed2837a8b4001f245cd5/bin/skillcli.exe'
$expected = '61AE7E3C113759CBC2BF1D85278E94E04C53B3F91CC0FDC8188FC12B1449DF34'
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
