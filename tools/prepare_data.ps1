<#
.SYNOPSIS
  Build the `ux0:data/mcsm/` data folder for the Minecraft: Story Mode Vita loader
  from your own legally-owned Android copy. You only supply the APK and the OBB(s).

.DESCRIPTION
  From the APK it pulls the five ARMv7 native libraries and the small `assets/` config
  files. The OBB expansion(s) are copied in whole and renamed to the filenames the loader
  expects (the engine mounts them and reads the game's `.ttarch2` archives directly). Empty
  runtime folders (saves etc.) are created too. The result is a ready-to-copy `mcsm/` folder.

  No game data is bundled with this script — you provide the APK and OBB yourself.

.EXAMPLE
  .\prepare_data.ps1 -Apk .\minecraft.apk -Obb .\obbs
  # -> writes .\mcsm\  ; copy that folder to ux0:data\ on the Vita (must stay named "mcsm")

.PARAMETER Apk
  Path to the game's ARMv7 APK.

.PARAMETER Obb
  A folder containing the OBB(s) (auto-detected), or the path to the main .obb directly.

.PARAMETER Out
  Output folder. Default: .\mcsm
#>
param(
    [Parameter(Mandatory = $true)][string]$Apk,
    [Parameter(Mandatory = $true)][string]$Obb,
    [string]$Out = "./mcsm"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not (Test-Path $Apk)) { throw "APK not found: $Apk" }
if (-not (Test-Path $Obb)) { throw "OBB path not found: $Obb" }

# The loader is hard-wired to these OBB names (java.c getObbFileName). Whatever version code
# your own OBBs carry, they're copied in under these exact names so the engine finds them.
$MAIN_NAME  = "main.40129.com.telltalegames.minecraft100.obb"
$PATCH_NAME = "patch.40135.com.telltalegames.minecraft100.obb"

# Resolve the OBBs: -Obb may be a folder (auto-detect main.*/patch.*) or the main .obb file.
if (Test-Path $Obb -PathType Container) {
    $mainObb  = Get-ChildItem $Obb -Filter "main.*.obb"  -File | Select-Object -First 1
    $patchObb = Get-ChildItem $Obb -Filter "patch.*.obb" -File | Select-Object -First 1
} else {
    $mainObb  = Get-Item $Obb
    $patchObb = Get-ChildItem (Split-Path $Obb) -Filter "patch.*.obb" -File -ErrorAction SilentlyContinue | Select-Object -First 1
}
if (-not $mainObb) { throw "No 'main.*.obb' found at: $Obb" }

$outDir = (New-Item -ItemType Directory -Path $Out -Force).FullName
Write-Host "Building data folder -> $outDir"
Write-Host ""

# 1. Native libraries + small assets/ config, straight out of the APK (a normal ZIP).
$libs = "libmain.so", "libGameEngine.so", "libSDL2.so", "libfmod.so", "libfmodstudio.so"
$apk = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $Apk).Path)
try {
    foreach ($lib in $libs) {
        $e = $apk.Entries | Where-Object { $_.FullName -eq "lib/armeabi-v7a/$lib" } | Select-Object -First 1
        if (-not $e) { throw "APK is missing lib/armeabi-v7a/$lib - is this the 32-bit (ARMv7) build?" }
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $outDir $lib), $true)
        Write-Host ("  lib     {0}" -f $lib)
    }
    $assets = $apk.Entries | Where-Object { $_.FullName -like "assets/*" -and $_.Name }
    foreach ($e in $assets) {
        $dest = Join-Path $outDir ($e.FullName -replace '/', '\')
        New-Item -ItemType Directory -Path (Split-Path $dest) -Force | Out-Null
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dest, $true)
    }
    Write-Host ("  assets  {0} config file(s) from APK" -f $assets.Count)
}
finally { $apk.Dispose() }

# 2. OBB expansion(s), copied whole under the loader's expected names.
Copy-Item $mainObb.FullName (Join-Path $outDir $MAIN_NAME) -Force
Write-Host ("  obb     {0}  ({1:N0} MB)" -f $MAIN_NAME, ($mainObb.Length / 1MB))
if ($patchObb) {
    Copy-Item $patchObb.FullName (Join-Path $outDir $PATCH_NAME) -Force
    Write-Host ("  obb     {0}  ({1:N0} MB)" -f $PATCH_NAME, ($patchObb.Length / 1MB))
} else {
    Write-Host "  obb     (no patch.*.obb found - continuing with main only)"
}

# 3. Empty runtime folders the engine writes into (saves, cache).
foreach ($d in "Temp", "User", "Net", "settings") {
    New-Item -ItemType Directory -Path (Join-Path $outDir $d) -Force | Out-Null
}

Write-Host ""
Write-Host "Done. Copy the whole '$outDir' folder to ux0:data\ on your Vita"
Write-Host "(it must stay named 'mcsm', i.e. ux0:data\mcsm\). Optional tuning files: data/mcsm/README.md."
