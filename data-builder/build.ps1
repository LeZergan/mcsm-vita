param(
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectFile = Join-Path $projectRoot "MCSMDataBuilder.csproj"
$testProject = Join-Path $projectRoot "tests\MCSMDataBuilder.SmokeTests.csproj"
$output = Join-Path $projectRoot "dist"

if (-not $SkipTests) {
    Write-Host "Running the data-folder smoke test..."
    dotnet run --project $testProject --configuration Release
    if ($LASTEXITCODE -ne 0) { throw "Smoke test failed." }
}

Write-Host "Publishing the self-contained Windows executable..."
dotnet publish $projectFile `
    --configuration Release `
    --runtime win-x64 `
    --self-contained true `
    -p:PublishSingleFile=true `
    -p:IncludeNativeLibrariesForSelfExtract=true `
    -p:EnableCompressionInSingleFile=true `
    --output $output
if ($LASTEXITCODE -ne 0) { throw "Publish failed." }

$exe = Join-Path $output "MCSM-Vita-Data-Builder.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "Expected executable was not created: $exe" }

$hash = Get-FileHash -LiteralPath $exe -Algorithm SHA256
Write-Host ""
Write-Host "Ready: $exe"
Write-Host "SHA256: $($hash.Hash)"
