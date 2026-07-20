param(
    [Parameter(Mandatory = $true)]
    [string]$VitaIp,

    [string]$OutputPath = "coredump"
)

$ErrorActionPreference = "Stop"

$baseUri = "ftp://$VitaIp`:1337/ux0:/data/"
$listing = curl.exe --silent $baseUri
if (-not $listing) {
    throw "Failed to list ux0:/data/ from $VitaIp"
}

$latest = ($listing -split "`r?`n" |
    Where-Object { $_ -match "psp2core" } |
    Select-Object -Last 1)

if (-not $latest) {
    throw "No psp2core file found on $VitaIp"
}

$parts = $latest -split "\s+"
$coreName = $parts[-1]
if (-not $coreName) {
    throw "Could not parse coredump filename from FTP listing line: $latest"
}

$coreUri = "$baseUri$coreName"
curl.exe --silent --output $OutputPath $coreUri
if (-not (Test-Path $OutputPath)) {
    throw "Failed to download coredump from $coreUri"
}

Write-Host "Downloaded $coreName to $OutputPath"
