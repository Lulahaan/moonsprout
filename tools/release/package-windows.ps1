# Packages a Windows build for a release: a portable zip and an Inno Setup installer.
#   pwsh tools/release/package-windows.ps1 <version> <build dir> <output dir>
# Run from the repository root. Installs Inno Setup with Chocolatey when ISCC.exe is missing.
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$OutDir
)
$ErrorActionPreference = 'Stop'
$name = "Moonsprout-$Version-windows-x64"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("moonsprout-" + [guid]::NewGuid())
$dir = Join-Path $work 'Moonsprout'
New-Item -ItemType Directory -Force $dir, (Join-Path $dir 'licenses') | Out-Null

# The program, the DLLs the build put next to it (Dawn's shader compiler), res/ and the notices.
Copy-Item (Join-Path $BuildDir 'mppm.exe') (Join-Path $dir 'Moonsprout.exe')
Get-ChildItem $BuildDir -Filter *.dll | Copy-Item -Destination $dir
Copy-Item -Recurse (Join-Path $BuildDir 'res') (Join-Path $dir 'res')
Copy-Item LICENSE.MD (Join-Path $dir 'LICENSE.txt')
Copy-Item port/launcher/res/fonts/licenses/*, port/libm/COPYRIGHT (Join-Path $dir 'licenses')
Copy-Item tools/release/README-release.txt (Join-Path $dir 'README.txt')
Get-ChildItem $dir | Format-Table Name, Length

# Installer (no portable.txt: an installed copy keeps its data in %APPDATA%\mppm).
$iscc = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
if (-not (Test-Path $iscc)) {
    choco install innosetup --no-progress -y | Out-Host
}
& $iscc /Qp "/DAppVersion=$Version" "/DSourceDir=$dir" "/DOutputName=$name-setup" packaging/windows/moonsprout.iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
Move-Item "packaging/windows/$name-setup.exe" (Join-Path $OutDir "$name-setup.exe") -Force

# Portable zip: the same folder plus portable.txt, so settings and saves stay in Moonsprout\data.
Set-Content -Path (Join-Path $dir 'portable.txt') -Value @(
    'This file makes Moonsprout portable: settings, saves and mods are kept in the "data" folder',
    'next to Moonsprout.exe. Delete it to use %APPDATA%\mppm instead.')
Compress-Archive -Path $dir -DestinationPath (Join-Path $OutDir "$name-portable.zip") -CompressionLevel Optimal -Force
Remove-Item -Recurse -Force $work
Get-ChildItem $OutDir | Format-Table Name, Length
