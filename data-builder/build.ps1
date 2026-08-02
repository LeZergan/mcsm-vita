param(
    [switch]$SkipTests,
    [string]$ButtonFixPath,
    [string]$ChoiceDataPath
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectFile = Join-Path $projectRoot "MCSMDataBuilder.csproj"
$testProject = Join-Path $projectRoot "tests\MCSMDataBuilder.SmokeTests.csproj"
$output = Join-Path $projectRoot "dist"
$localAssets = Join-Path $projectRoot "LocalAssets"
$buttonFixPack = Join-Path $localAssets "button-fix.zip"
$choiceData = Join-Path $localAssets "choice.prop"
$appIcon = Join-Path $localAssets "app-icon.ico"
$iconPreview = Join-Path $output "app-icon-preview.png"

& (Join-Path $projectRoot "generate-icon.ps1") -OutputPath $appIcon -PreviewPath $iconPreview
Write-Host "Generated the branded builder icon."

if ($ChoiceDataPath) {
    $resolvedChoice = Resolve-Path -LiteralPath $ChoiceDataPath -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $resolvedChoice.Path -PathType Leaf)) {
        throw "ChoiceDataPath must be a choice.prop file: $ChoiceDataPath"
    }
    if (-not [System.IO.Path]::GetFileName($resolvedChoice.Path).Equals("choice.prop", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "ChoiceDataPath must point to a file named choice.prop."
    }
    $choiceHash = (Get-FileHash -LiteralPath $resolvedChoice.Path -Algorithm SHA256).Hash
    $expectedChoiceHash = "F5F0C7FF7467707C7224BF056C6F7111E8D27279AA0BEE3BA422886B7EBB2616"
    if (-not $choiceHash.Equals($expectedChoiceHash, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "The selected choice.prop is not the supported offline crowd-choice dataset."
    }
    New-Item -ItemType Directory -Path $localAssets -Force | Out-Null
    if (-not $resolvedChoice.Path.Equals($choiceData, [System.StringComparison]::OrdinalIgnoreCase)) {
        Copy-Item -LiteralPath $resolvedChoice.Path -Destination $choiceData -Force
    }
    Write-Host "Bundled offline choice statistics: choice.prop"
} elseif (Test-Path -LiteralPath $choiceData) {
    $choiceHash = (Get-FileHash -LiteralPath $choiceData -Algorithm SHA256).Hash
    if (-not $choiceHash.Equals("F5F0C7FF7467707C7224BF056C6F7111E8D27279AA0BEE3BA422886B7EBB2616", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "LocalAssets/choice.prop is not the supported offline crowd-choice dataset."
    }
    Write-Host "Using existing local offline choice dataset."
} else {
    Write-Warning "No offline choice dataset is bundled. Pass -ChoiceDataPath with the supported choice.prop to include crowd statistics."
}

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

$hash = $null
for ($attempt = 1; $attempt -le 8; $attempt++) {
    try {
        $hash = Get-FileHash -LiteralPath $exe -Algorithm SHA256 -ErrorAction Stop
        break
    } catch [System.IO.IOException] {
        if ($attempt -eq 8) { throw }
        # Windows Defender can briefly retain a newly published single-file EXE.
        Start-Sleep -Milliseconds 500
    }
}
Write-Host ""
Write-Host "Ready: $exe"
Write-Host "SHA256: $($hash.Hash)"
