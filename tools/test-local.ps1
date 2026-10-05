$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -version '[16.0,17.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio 2019 C++ tools (v142) are required.' }
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$repo\tests\shadow_bones.vcxproj" /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'Test build failed.' }
& "$repo\build\tests\shadow_bones.exe"
if ($LASTEXITCODE -ne 0) { throw 'Shadow bone regression tests failed.' }
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$repo\tests\lut_identity.vcxproj" /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'LUT identity test build failed.' }
Push-Location $repo
try { & "$repo\build\lut-test\lut_identity.exe"; if ($LASTEXITCODE -ne 0) { throw 'LUT identity tests failed.' } } finally { Pop-Location }
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$repo\tests\interior_shadow_math.vcxproj" /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'Interior shadow math test build failed.' }
& "$repo\build\interior-test\interior_shadow_math.exe"
if ($LASTEXITCODE -ne 0) { throw 'Interior shadow math tests failed.' }
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$repo\tests\point_shadow_schedule.vcxproj" /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'Point shadow schedule test build failed.' }
& "$repo\build\schedule-test\point_shadow_schedule.exe"
if ($LASTEXITCODE -ne 0) { throw 'Point shadow schedule tests failed.' }
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$repo\tests\frame_stats.vcxproj" /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'Frame statistics test build failed.' }
& "$repo\build\frame-test\frame_stats.exe"
if ($LASTEXITCODE -ne 0) { throw 'Frame statistics tests failed.' }
