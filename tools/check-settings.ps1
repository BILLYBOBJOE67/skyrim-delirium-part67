# The settings check: loads every plugin\tests\settings\*.ini the way the core does (SettingsCheck.exe) and compares
# every setting and reaction row with the case's .expected.txt. Run it after any change to how settings are read.
#   tools\check-settings.ps1           build, run, compare (exit 1 on any difference)
#   tools\check-settings.ps1 -Record   write the .expected.txt files from the current code (only when the code is known good)
param([switch]$Record)
$ErrorActionPreference = 'Stop'
$plugin = Join-Path $PSScriptRoot '..\plugin' | Resolve-Path
$cases = Join-Path $plugin 'tests\settings'
$exe = Join-Path $plugin 'build\Release\SettingsCheck.exe'

$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$log = Join-Path $env:TEMP 'SettingsCheck.build.log'
& cmd /c "`"$vcvars`" >nul && cmake --build `"$plugin\build\Release`" --target SettingsCheck" *> $log
if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 30; Write-Host "BUILD FAILED (run plugin\build.cmd Release once first)"; exit 1 }

$work = Join-Path $env:TEMP 'SettingsCheck'
$failed = 0
foreach ($ini in Get-ChildItem $cases -Filter *.ini | Sort-Object Name) {
	$dir = Join-Path $work $ini.BaseName
	$plugins = Join-Path $dir 'Data\SKSE\Plugins'
	New-Item -ItemType Directory -Force $plugins | Out-Null
	Copy-Item $ini.FullName (Join-Path $plugins 'SkyrimDelirium.ini') -Force
	$got = Join-Path $dir 'got.txt'
	& $exe $dir $got
	if ($LASTEXITCODE -ne 0) { Write-Host "FAIL $($ini.Name): SettingsCheck exit $LASTEXITCODE"; $failed++; continue }
	$expected = Join-Path $cases "$($ini.BaseName).expected.txt"
	if ($Record) { Copy-Item $got $expected -Force; Write-Host "recorded $($ini.Name)"; continue }
	if (-not (Test-Path $expected)) { Write-Host "FAIL $($ini.Name): no $($ini.BaseName).expected.txt (run with -Record)"; $failed++; continue }
	$diff = Compare-Object (Get-Content $expected) (Get-Content $got)
	if ($diff) {
		Write-Host "FAIL $($ini.Name): $($diff.Count) lines differ (<= expected, => now)"
		$diff | Select-Object -First 12 | ForEach-Object { Write-Host "  $($_.SideIndicator) $($_.InputObject)" }
		$failed++
	} else {
		Write-Host "ok   $($ini.Name)"
	}
}
if ($failed) { Write-Host "$failed case(s) FAILED"; exit 1 }
if (-not $Record) { Write-Host 'settings check passed' }
