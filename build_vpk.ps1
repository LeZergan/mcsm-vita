$ErrorActionPreference = 'Stop'

# Point VITASDK at your softfp VitaSDK (see https://github.com/vitasdk-softfp/vdpm).
if (-not $env:VITASDK) {
    throw "VITASDK is not set. Point it at your softfp VitaSDK, e.g.  set VITASDK=C:\vitasdk"
}
$sdkRoot   = $env:VITASDK
$toolchain = Join-Path $sdkRoot 'share\vita.toolchain.cmake'
$gcc       = Join-Path $sdkRoot 'bin\arm-vita-eabi-gcc.exe'
if (-not (Test-Path $toolchain) -or -not (Test-Path $gcc)) {
    throw "Softfp VitaSDK not found under $sdkRoot (need share/vita.toolchain.cmake and bin/arm-vita-eabi-gcc.exe)."
}
$env:PATH = "$sdkRoot\bin;$env:PATH"

$buildDir = Join-Path $PSScriptRoot 'build_local'

# Optional: crush the LiveArea PNGs if pngquant is on PATH (Vita install validation is
# picky about PNG format). Skipped silently if pngquant isn't installed.
$pngquantCmd = Get-Command pngquant -ErrorAction SilentlyContinue
if ($pngquantCmd) {
    $liveareaDir = Join-Path $PSScriptRoot 'extras\livearea'
    foreach ($name in @('icon0.png', 'pic0.png', 'startup.png', 'bg0.png')) {
        $png = Join-Path $liveareaDir $name
        if (Test-Path $png) {
            & $pngquantCmd.Source --force --strip --speed 1 --quality 0-100 --ext .png -- $png
            if ($LASTEXITCODE -ne 0) { throw "pngquant failed for $png (exit $LASTEXITCODE)" }
        }
    }
}

# Logging is env-driven: OFF compiles out all runtime logging (production / perf builds).
$logOpt = if ($env:ENABLE_TELEMETRY_LOGGING) { $env:ENABLE_TELEMETRY_LOGGING } else { 'ON' }
Write-Host "[build] ENABLE_TELEMETRY_LOGGING=$logOpt"

cmake -S $PSScriptRoot -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHADER_FORMAT=GLSL "-DENABLE_TELEMETRY_LOGGING=$logOpt"
cmake --build $buildDir --parallel

Write-Host "Built: $(Join-Path $buildDir 'mcsm_diag.vpk')"
