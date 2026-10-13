param([switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools are required.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Install the Desktop development with C++ workload.' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$build = Join-Path $PSScriptRoot 'build'
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Path $build,$dist -Force | Out-Null
& (Join-Path $PSScriptRoot 'native\Generate-Icon.ps1')
$compile = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
rc /nologo /fo "build\Setup.res" native\Setup.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 /O2 /MT native\Setup.cpp build\Setup.res /Fo"build\Setup.obj" /Fe"build\Setup.exe" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"native\app.manifest" user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib uuid.lib dwmapi.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 /O2 /MT /DOLED_OFF_TEST native\Setup.cpp build\Setup.res /Fo"build\SetupTests.obj" /Fe"build\SetupTests.exe" /link /SUBSYSTEM:CONSOLE /MANIFEST:EMBED /MANIFESTINPUT:"native\app.manifest" user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib uuid.lib gdiplus.lib dwmapi.lib
if errorlevel 1 exit /b 1
build\SetupTests.exe
exit /b %ERRORLEVEL%
"@
$compilePath = Join-Path $build 'compile.cmd'
[IO.File]::WriteAllText($compilePath, $compile, [Text.Encoding]::Default)
Push-Location $PSScriptRoot
try {
    & $env:ComSpec /d /c ('"' + $compilePath + '"')
    if ($LASTEXITCODE -ne 0) { throw 'C++ build or verification failed.' }
    if ($BuildOnly) {
        Write-Output "Verified build: $build\Setup.exe"
        return
    }
    Copy-Item -LiteralPath (Join-Path $build 'Setup.exe') -Destination (Join-Path $dist 'Setup.exe') -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native\README-ja.md') -Destination (Join-Path $dist 'README.md')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE') -Destination (Join-Path $dist 'LICENSE')
    Compress-Archive -LiteralPath (Join-Path $dist 'Setup.exe'),(Join-Path $dist 'README.md'),(Join-Path $dist 'LICENSE') -DestinationPath (Join-Path $dist 'OLED_OFF-Windows-x64.zip') -Force
    $packageHash = (Get-FileHash -LiteralPath (Join-Path $dist 'OLED_OFF-Windows-x64.zip') -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText((Join-Path $dist 'SHA256SUMS.txt'), "$packageHash  OLED_OFF-Windows-x64.zip`n", [Text.Encoding]::ASCII)
    Write-Output "Built: $dist\Setup.exe"
    Write-Output "Package: $dist\OLED_OFF-Windows-x64.zip"
    Write-Output "Checksum: $dist\SHA256SUMS.txt"
}
finally { Pop-Location }
