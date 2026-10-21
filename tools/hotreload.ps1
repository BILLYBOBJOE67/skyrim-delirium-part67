# While the game runs: builds, copies the core (SkyrimDeliriumCore.dll) into the mod folder and tells the host
# to swap it in. Host changes (main.cpp, core_api.h) still need a game restart and tools\install.ps1.
#   hotreload.ps1                  build + reload
#   hotreload.ps1 -Cmd "push FF00098D 120"   only send command lines (separate several with ';')
param(
	[string]$Mod = 'C:\path\to\MO2\mods\Skyrim Delirium',
	[string]$Cmd = '',
	[switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$skse = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Skyrim Special Edition\SKSE'

function Send([string[]]$lines) {
	$tmp = Join-Path $skse 'SkyrimDelirium.cmd.tmp'
	$dst = Join-Path $skse 'SkyrimDelirium.cmd'
	for ($i = 0; (Test-Path $dst) -and $i -lt 50; $i++) { Start-Sleep -Milliseconds 100 }  # the last one not picked up yet
	Set-Content -Path $tmp -Value $lines -Encoding ascii
	Move-Item -Force $tmp $dst
}

if ($Cmd) {
	Send ($Cmd -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
	Write-Output "SENT: $Cmd"
	exit 0
}
if (-not $NoBuild) {
	& "$root\plugin\build.cmd" Release | Select-String -Pattern ' error |FAILED|LNK1' | ForEach-Object { $_.Line }
	if ($LASTEXITCODE -ne 0) { Write-Output 'BUILD FAILED'; exit 1 }
}
foreach ($ext in 'dll', 'pdb') {
	$src = "$root\plugin\build\Release\SkyrimDeliriumCore.$ext"
	$dst = Join-Path $Mod "SKSE\Plugins\SkyrimDelirium\SkyrimDeliriumCore.$ext"
	Copy-Item $src $dst -Force
	if ((Get-FileHash $src).Hash -ne (Get-FileHash $dst).Hash) { Write-Output "HASH MISMATCH: $dst"; exit 1 }
}
if (-not (Get-Process SkyrimSE -ErrorAction SilentlyContinue)) { Write-Output 'COPIED (game not running, no reload)'; exit 0 }
Send @('reload')
Write-Output 'COPIED + reload sent'
