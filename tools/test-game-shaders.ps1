# Game shader A/B (tests/game_shaders.cpp): the committed game shaders (git HEAD, or -Base <commit>) against the working
# copy. Compiles both with the game's D3DX43 compiler, requires bit-identical pixels on the GPU and times both.
# -Only terrain|objects runs one part. Needs a D3D9 GPU.
param([string]$Base = 'HEAD', [string]$Only = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -version '[16.0,17.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio 2019 C++ tools (v142) are required.' }
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$repo\tests\game_shaders.vcxproj" /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'Game shader test build failed.' }
$old = "$repo\build\shader-test\old"
if (Test-Path $old) { Remove-Item -Recurse -Force $old }
New-Item -ItemType Directory -Force $old | Out-Null
$archive = "$repo\build\shader-test\old.tar"
& git -C $repo -c safe.directory=* archive -o $archive $Base src/hlsl/NewVegas/Shaders
if ($LASTEXITCODE -ne 0) { throw "git archive $Base failed" }
& tar -xf $archive -C $old
if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
$arguments = @("$old\src\hlsl\NewVegas\Shaders", "$repo\src\hlsl\NewVegas\Shaders")
if ($Only) { $arguments += $Only }
& "$repo\build\shader-test\game_shaders.exe" @arguments
if ($LASTEXITCODE -ne 0) { throw 'Game shader checks failed.' }
