# Real-host test: renders a generated song through a VST3 plugin with the songengine Go VST3 host
# (C:\Users\stora\Downloads\VSTHostSynthesizerEngine), offline, and checks the result.
# Every song part is played by the plugin, so no other synths are needed.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\host_render.ps1                       # tonic2_poly.vst3
#   powershell -ExecutionPolicy Bypass -File tools\host_render.ps1 -Plugin path\to\x.vst3 -Out x.wav
# Override the host location with -SongEngine or the TONIC2_SONGENGINE environment variable.
param(
  [string]$Plugin = "",
  [string]$Out = "",
  [string]$Seed = "0x1234",
  [string]$Genre = "house",
  [int]$Bars = 8,
  [string]$SongEngine = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Plugin) { $Plugin = Join-Path $root "build-win\bin\tonic2_poly.vst3" }
if (-not $Out) { $Out = Join-Path $root ("build-win\host_" + [IO.Path]::GetFileNameWithoutExtension($Plugin) + ".wav") }
if (-not $SongEngine) { $SongEngine = $env:TONIC2_SONGENGINE }
if (-not $SongEngine) { $SongEngine = "C:\Users\stora\Downloads\VSTHostSynthesizerEngine\songengine\dist\songengine-windows-amd64.exe" }
if (-not (Test-Path $Plugin)) { throw "plugin not found: $Plugin (run tools\build_windows.ps1 first)" }
if (-not (Test-Path $SongEngine)) { throw "songengine host not found: $SongEngine" }

$argList = @("-seed", $Seed, "-genre", $Genre, "-channels", "4", "-bars", "$Bars", "-wav", $Out)
foreach ($role in "drums", "perc", "bass", "lead", "keys", "pad", "arp", "noise") { $argList += @("-plugin", "$role=$Plugin") }
if (Test-Path $Out) { Remove-Item $Out }

# DPF prints a soft assert ("outparamsptr != nullptr") each block because the host passes no output
# parameter-change list to process(); that is allowed by VST3 and harmless, so it is filtered here.
$ErrorActionPreference = "Continue"  # PowerShell 5.1 turns native stderr lines into errors under Stop
$log = & $SongEngine @argList 2>&1 | ForEach-Object { "$_" }
$exit = $LASTEXITCODE
$ErrorActionPreference = "Stop"
$log | Where-Object { $_ -notmatch "outparamsptr|DistrhoPluginVST3\.cpp|^\s*\[dpf\]\s*$|RemoteException" } | Select-Object -Last 10
if ($exit -ne 0) { throw "songengine exited with $exit" }
if (-not (Test-Path $Out)) { throw "no output written" }

$peakLine = $log | Select-String -Pattern "peak ([0-9.]+)" | Select-Object -Last 1
$peak = if ($peakLine) { [double]$peakLine.Matches[0].Groups[1].Value } else { -1 }
$size = (Get-Item $Out).Length
"host render: $Out ($size bytes, peak $peak)"
if ($peak -le 0.001 -or $peak -ge 1.0) { throw "suspicious level: peak $peak" }
"PASS: plugin loaded and rendered in a real VST3 host"
