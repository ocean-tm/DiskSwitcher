param([string]$OutputName = 'DiskSwitcher.exe')
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force -Path build | Out-Null
    & windres app.rc -O coff -o build/app.res
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed' }
    & gcc main.c power.c build/app.res -o $OutputName -std=c11 -Os -s -Wall -Wextra -Werror -municode -mwindows -static -lsetupapi -ladvapi32 -luser32 -lgdi32 '-Wl,--nxcompat' '-Wl,--dynamicbase'
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
    Get-Item -LiteralPath $OutputName | Select-Object FullName, Length
} finally { Pop-Location }
