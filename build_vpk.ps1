$ErrorActionPreference = 'Stop'

# Point VITASDK at your softfp VitaSDK (see https://github.com/vitasdk-softfp/vdpm).
if (-not $env:VITASDK) {
    throw "VITASDK is not set. Point it at your softfp VitaSDK, e.g.  set VITASDK=C:\vitasdk"
}
$sdkRoot   = $env:VITASDK
$toolchain = Join-Path $sdkRoot 'share\vita.toolchain.cmake'
$gcc       = Join-Path $sdkRoot 'bin\arm-vita-eabi-gcc.exe'
$readelf   = Join-Path $sdkRoot 'bin\arm-vita-eabi-readelf.exe'
$vitaGl    = Join-Path $sdkRoot 'arm-vita-eabi\lib\libvitaGL.a'
if (-not (Test-Path $toolchain) -or -not (Test-Path $gcc) -or
    -not (Test-Path $readelf) -or -not (Test-Path $vitaGl)) {
    throw "Softfp VitaSDK not found under $sdkRoot (need toolchain, compiler, readelf, and libvitaGL.a)."
}

# Refuse the regular hard-float SDK. The loader and Android modules are softfp;
# suppressing this mismatch can produce a clean-looking but ABI-unsafe VPK.
$vitaGlAttrs = (& $readelf -A $vitaGl 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "Could not inspect vitaGL ABI at $vitaGl"
}
if ($vitaGlAttrs -match 'Tag_ABI_VFP_args:\s*VFP registers') {
    throw "Hard-float vitaGL detected at $vitaGl. Set VITASDK to the softfp SDK."
}
$vitaGlHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $vitaGl).Hash
Write-Host "[build] vitaGL SHA256=$vitaGlHash"
$env:PATH = "$sdkRoot\bin;$env:PATH"

$buildDir = Join-Path $PSScriptRoot 'build_local'

# Logging is opt-in: production/performance builds compile it out by default.
$logOpt = if ($env:ENABLE_TELEMETRY_LOGGING) { $env:ENABLE_TELEMETRY_LOGGING } else { 'OFF' }
Write-Host "[build] ENABLE_TELEMETRY_LOGGING=$logOpt"

cmake --fresh -S $PSScriptRoot -B $buildDir -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $PSScriptRoot 'cmake\vita.toolchain.wrapper.cmake')" `
    "-DVITA_REAL_TOOLCHAIN_FILE=$toolchain" `
    -DCMAKE_BUILD_TYPE=Release -DSHADER_FORMAT=GLSL `
    "-DENABLE_TELEMETRY_LOGGING=$logOpt"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }
cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)" }

Write-Host "Built: $(Join-Path $buildDir 'mcsm_diag.vpk')"
