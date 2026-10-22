# Builds the plugin and lays the mod out in an MO2 mod folder:
#   SKSE\Plugins\SkyrimDelirium.dll, SkyrimDelirium.ini
#   SKSE\Plugins\SkyrimDelirium\delirium_physics.dll, skyrim_build.txt, skyrim_rig.txt
# The body and rig are made from the user's own GTA IV data (local\): local use only, never packaged for release.
param(
	[string]$Mod = '',
	[string]$Delirium = '',
	[switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
# folders not given: paths.local.json at the repo root (copy paths.example.json)
$paths = Join-Path $root 'paths.local.json'
$local = if (Test-Path $paths) { Get-Content $paths -Raw | ConvertFrom-Json } else { $null }
if (-not $Mod -and $local) { $Mod = $local.mod }
if (-not $Delirium -and $local) { $Delirium = $local.delirium }
if (-not $Mod -or -not $Delirium) { Write-Output 'NO PATHS: pass -Mod and -Delirium, or copy paths.example.json to paths.local.json'; exit 3 }

if (-not $NoBuild) {
	& "$root\plugin\build.cmd" Release
	if ($LASTEXITCODE -ne 0) { Write-Output 'BUILD FAILED'; exit 1 }
}

$plugins = Join-Path $Mod 'SKSE\Plugins'
$data = Join-Path $plugins 'SkyrimDelirium'
New-Item -ItemType Directory -Force $data | Out-Null

$copy = @(
	@("$root\plugin\build\Release\SkyrimDelirium.dll", $plugins),
	@("$root\plugin\build\Release\SkyrimDeliriumCore.dll", $data),
	@("$root\plugin\build\Release\SkyrimDeliriumCore.pdb", $data),
	@("$Delirium\target\release\delirium_physics.dll", $data),
	@("$root\local\skyrim_build.txt", $data),
	@("$root\local\skyrim_rig.txt", $data)
)
foreach ($c in $copy) {
	if (-not (Test-Path $c[0])) { Write-Output "MISSING: $($c[0])"; exit 1 }
	Copy-Item $c[0] $c[1] -Force
	$dst = Join-Path $c[1] (Split-Path $c[0] -Leaf)
	if ((Get-FileHash $c[0]).Hash -ne (Get-FileHash $dst).Hash) { Write-Output "HASH MISMATCH: $dst"; exit 1 }
}
# the ini: only when missing, so settings changed in the mod folder survive
$ini = Join-Path $plugins 'SkyrimDelirium.ini'
if (-not (Test-Path $ini)) { Copy-Item "$root\plugin\data\SkyrimDelirium.ini" $ini }
Write-Output "DEPLOYED to ${Mod}: $($copy.Count) files hash-verified (+ ini)"
