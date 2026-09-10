[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateLength(1, 511)]
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$')]
    [string]$Source
)

$ErrorActionPreference = 'Stop'
$uri = 'https://raw.githubusercontent.com/WillEastbury/skillcli/94b5945b714edcc887a8ee03786cce13d5a06ced/bin/skillcli.exe'
$expected = '35F53898B1EC3C822CEA7846C7F7D5082BBD4BCD88269C62112C74911579E782'
$directory = Join-Path $env:LOCALAPPDATA 'SkillCLI\Bootstrap'
$installer = Join-Path $directory 'skillcli.exe'

try {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    Invoke-WebRequest $uri -OutFile $installer
    if ((Get-FileHash $installer -Algorithm SHA256).Hash -ne $expected) {
        throw 'Frontier Agent Helper checksum verification failed.'
    }
    Unblock-File -LiteralPath $installer
    & $installer setup --source $Source
    if ($LASTEXITCODE) {
        throw "Frontier Agent Helper setup failed with exit code $LASTEXITCODE."
    }
} finally {
    Remove-Item $installer -Force -ErrorAction SilentlyContinue
}
