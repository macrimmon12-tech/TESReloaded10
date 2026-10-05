$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -version '[16.0,17.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio 2019 C++ tools (v142) are required.' }
# Build only: never deploy to the game or delete an existing directory.
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$repo\NewVegasReloaded\NewVegasReloaded.vcxproj" /p:Configuration=Release /p:Platform=Win32 "/p:SolutionDir=$repo\" "/p:OutDir=$repo\build\optimized\" "/p:IntDir=$repo\build\optimized-obj\" /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'New Vegas Reloaded build failed.' }
