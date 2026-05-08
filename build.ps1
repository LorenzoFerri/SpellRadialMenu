param([switch]$Ninja)

$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$BuildDir = Join-Path $ProjectRoot 'build'
$VendorDir = Join-Path $ProjectRoot 'vendor'
$ImguiDir = Join-Path $VendorDir 'imgui'
$MinHookDir = Join-Path $VendorDir 'minhook'

$ImguiRepo = 'https://github.com/ocornut/imgui.git'
$ImguiRef = 'a70b97ee48b5a83fc3aa4dec479d64233b715363'
$MinHookRepo = 'https://github.com/TsudaKageyu/minhook.git'
$MinHookRef = '05c06c5bbca226b72ffb40fc0caaef33bcaf6f74'

function Require-Command($Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Missing required command: $Name"
    }
}

function Ensure-Vendor($Name, $Repo, $Ref, $Dir, $CheckFile) {
    if (Test-Path (Join-Path $Dir $CheckFile)) {
        return
    }

    Require-Command git
    if (Test-Path $Dir) {
        Remove-Item -Recurse -Force $Dir
    }

    git clone --filter=blob:none --no-checkout $Repo $Dir
    git -C $Dir fetch --depth 1 origin $Ref
    git -C $Dir checkout --detach FETCH_HEAD

    if (-not (Test-Path (Join-Path $Dir $CheckFile))) {
        throw "Failed to prepare $Name in $Dir"
    }
}

Require-Command cmake

New-Item -ItemType Directory -Force -Path $BuildDir, $VendorDir, (Join-Path $ProjectRoot 'natives') | Out-Null

Ensure-Vendor 'ImGui' $ImguiRepo $ImguiRef $ImguiDir 'imgui.cpp'
Ensure-Vendor 'MinHook' $MinHookRepo $MinHookRef $MinHookDir 'include/MinHook.h'

$GeneratorArgs = @('-A', 'x64')
if ($Ninja) {
    Require-Command ninja
    $GeneratorArgs = @('-G', 'Ninja')
}

cmake -S $ProjectRoot -B $BuildDir @GeneratorArgs -DCMAKE_BUILD_TYPE=Release
cmake --build $BuildDir --config Release

Write-Host ''
Write-Host 'Built DLL:'
Write-Host "  $(Join-Path $ProjectRoot 'natives/RadialMenu.dll')"
