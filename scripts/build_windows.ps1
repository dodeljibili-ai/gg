$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git was not found.' }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'CMake was not found.' }
if (-not (Get-Command msbuild -ErrorAction SilentlyContinue)) { throw 'MSBuild was not found.' }

$sdk = Join-Path $root 'third_party\plugin-sdk'
$sdkCommit = '15f15b60bbf74c106e1b496ff92c98764abf4605'

if (-not (Test-Path (Join-Path $sdk 'shared\plugin.h'))) {
    if (Test-Path $sdk) { Remove-Item -Recurse -Force $sdk }
    New-Item -ItemType Directory -Force -Path (Split-Path $sdk) | Out-Null
    git init $sdk
    git -C $sdk remote add origin https://github.com/DK22Pac/plugin-sdk.git
    git -C $sdk fetch --depth 1 origin $sdkCommit
    git -C $sdk checkout --detach FETCH_HEAD
    git -C $sdk submodule update --init --recursive
}

if (-not (Test-Path (Join-Path $sdk 'shared\plugin.h'))) {
    throw "plugin-sdk was not installed correctly: $sdk"
}

$help = & cmake --help 2>$null | Out-String
if ($help -match 'Visual Studio 18 2026') {
    $generator = 'Visual Studio 18 2026'
} else {
    throw 'The current pinned plugin-sdk requires the Visual Studio 18 2026 generator for this build script.'
}

$gen = Join-Path $sdk 'tools\generate\Visual Studio.bat'
Push-Location $sdk
try {
    cmd /c "tools\generate\Visual Studio.bat"
    if ($LASTEXITCODE -ne 0) { throw "plugin-sdk solution generation failed: $LASTEXITCODE" }
}
finally {
    Pop-Location
}

$sdkSolution = Join-Path $sdk 'plugin.slnx'
if (-not (Test-Path $sdkSolution)) { throw "plugin-sdk solution not found: $sdkSolution" }

msbuild $sdkSolution /property:Configuration=Release /property:Platform="Mixed Platforms" /target:plugin_sa /m /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw "plugin-sdk build failed: $LASTEXITCODE" }

$pluginLib = Join-Path $sdk 'output\lib\Plugin.lib'
if (-not (Test-Path $pluginLib)) { throw "Plugin.lib was not produced: $pluginLib" }

# Download the same official Microsoft D3DX package used by CI.
$d3dxRoot = Join-Path $root 'third_party\D3DX'
$d3dxVersion = '9.29.952.8'
$nupkg = Join-Path $root "third_party\Microsoft.DXSDK.D3DX.$d3dxVersion.nupkg"
if (Test-Path $d3dxRoot) { Remove-Item -Recurse -Force $d3dxRoot }
New-Item -ItemType Directory -Force -Path $d3dxRoot | Out-Null
Invoke-WebRequest -Uri "https://www.nuget.org/api/v2/package/Microsoft.DXSDK.D3DX/$d3dxVersion" -OutFile $nupkg -UseBasicParsing
$zip = "$nupkg.zip"
Copy-Item $nupkg $zip
Expand-Archive -Path $zip -DestinationPath $d3dxRoot -Force
Remove-Item $nupkg,$zip -Force

$d3dxLib = Get-ChildItem $d3dxRoot -Recurse -File -Filter 'd3dx9.lib' |
    Where-Object { $_.FullName -match '\\build\\native\\release\\lib\\x86\\d3dx9\.lib$' } |
    Select-Object -First 1
$d3dxDll = Get-ChildItem $d3dxRoot -Recurse -File -Filter 'D3DX9_43.dll' |
    Where-Object { $_.FullName -match '\\build\\native\\release\\bin\\x86\\D3DX9_43\.dll$' } |
    Select-Object -First 1
$d3dCompiler = Get-ChildItem $d3dxRoot -Recurse -File -Filter 'D3DCompiler_43.dll' |
    Where-Object { $_.FullName -match '\\build\\native\\release\\bin\\x86\\D3DCompiler_43\.dll$' } |
    Select-Object -First 1
if (-not $d3dxLib -or -not $d3dxDll -or -not $d3dCompiler) { throw 'Required x86 D3DX files were not found.' }

$build = Join-Path $root 'build'
if (Test-Path $build) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force -Path $build | Out-Null

cmake `
    -S src `
    -B $build `
    -G $generator `
    -A Win32 `
    "-DPLUGIN_SDK_DIR=$sdk" `
    "-DD3DX9_LIBRARY=$($d3dxLib.FullName)" `
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

cmake --build $build --config Release --target AI_NPC -- /m
if ($LASTEXITCODE -ne 0) { throw "AI_NPC build failed: $LASTEXITCODE" }

$out = Join-Path $build 'Release\AI_NPC.asi'
if (-not (Test-Path $out)) { throw "AI_NPC.asi was not found: $out" }

$release = Join-Path $root 'release'
if (Test-Path $release) { Remove-Item -Recurse -Force $release }
New-Item -ItemType Directory -Force -Path $release | Out-Null
Copy-Item $out (Join-Path $release 'AI_NPC.asi')
Copy-Item $d3dxDll.FullName (Join-Path $release 'D3DX9_43.dll')
Copy-Item $d3dCompiler.FullName (Join-Path $release 'D3DCompiler_43.dll')
Copy-Item (Join-Path $root 'config\AI_NPC.ini') (Join-Path $release 'AI_NPC.ini')

Write-Host 'SUCCESS: ready-to-install files are in release\' -ForegroundColor Green
