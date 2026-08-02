param(
    [switch]$SkipTests,
    [string]$ButtonFixPath
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectFile = Join-Path $projectRoot "MCSMDataBuilder.csproj"
$testProject = Join-Path $projectRoot "tests\MCSMDataBuilder.SmokeTests.csproj"
$output = Join-Path $projectRoot "dist"
$localAssets = Join-Path $projectRoot "LocalAssets"
$buttonFixPack = Join-Path $localAssets "button-fix.zip"

if ($ButtonFixPath) {
    $resolvedFix = Resolve-Path -LiteralPath $ButtonFixPath -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $resolvedFix.Path -PathType Container)) {
        throw "ButtonFixPath must be a folder: $ButtonFixPath"
    }

    $fixFiles = @(Get-ChildItem -LiteralPath $resolvedFix.Path -Recurse -File)
    $unsupported = @($fixFiles | Where-Object { $_.Extension -notin @(".d3dtx", ".d3dmesh") })
    if ($unsupported.Count -gt 0) {
        throw "Button-fix folder contains unsupported files: $($unsupported.Name -join ', ')"
    }
    if ($fixFiles.Count -eq 0) { throw "Button-fix folder is empty." }

    $duplicates = @($fixFiles | Group-Object Name | Where-Object Count -gt 1)
    if ($duplicates.Count -gt 0) {
        throw "Button-fix folder has duplicate filenames: $($duplicates.Name -join ', ')"
    }

    New-Item -ItemType Directory -Path $localAssets -Force | Out-Null
    if (Test-Path -LiteralPath $buttonFixPack) {
        Remove-Item -LiteralPath $buttonFixPack -Force
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $resolvedFix.Path,
        $buttonFixPack,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false)
    Write-Host "Bundled controller button fix: $($fixFiles.Count) files"
} elseif (Test-Path -LiteralPath $buttonFixPack) {
    Write-Host "Using existing local controller button-fix package."
} else {
    Write-Warning "No controller button fix is bundled. Pass -ButtonFixPath with your local fix folder to include it."
}

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
