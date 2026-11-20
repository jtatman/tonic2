# Builds tonic2 (tests, tools) and the DPF plugins with the installed Visual Studio 2022, then runs ctest.
# Output: build-win\bin\tonic2_poly.{vst3,clap,lv2}
# Usage:  powershell -ExecutionPolicy Bypass -File tools\build_windows.ps1 [-Config Release] [-NoTests]
param(
  [string]$Config = "Release",
  [switch]$NoTests
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vsCmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$cmake = if (Get-Command cmake -ErrorAction SilentlyContinue) { "cmake" } else { Join-Path $vsCmake "cmake.exe" }
$ctest = if (Get-Command ctest -ErrorAction SilentlyContinue) { "ctest" } else { Join-Path $vsCmake "ctest.exe" }

if (-not (Test-Path (Join-Path $root "third_party\DPF\dgl\src\pugl-upstream\include")) -or
    -not (Test-Path (Join-Path $root "third_party\miniaudio\miniaudio.h"))) {
  git -C $root submodule update --init --recursive
}
& $cmake -S $root -B (Join-Path $root "build-win") -G "Visual Studio 17 2022" -A x64 -DTONIC2_BUILD_DPF=ON -DTONIC2_BUILD_TESTS=ON -DTONIC2_BUILD_STANDALONE=ON
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& $cmake --build (Join-Path $root "build-win") --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }
if (-not $NoTests) {
  & $ctest --test-dir (Join-Path $root "build-win") -C $Config --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "tests failed" }
}
Get-ChildItem (Join-Path $root "build-win\bin") | ForEach-Object { "built: $($_.FullName)" }
