param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [string]$PreviewPath
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
if (-not ("NativeMethods.IconHandle" -as [type])) {
    Add-Type -Namespace NativeMethods -Name IconHandle -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool DestroyIcon(System.IntPtr handle);
"@
}

function New-RoundedRectangle {
    param(
        [float]$X,
        [float]$Y,
        [float]$Width,
        [float]$Height,
        [float]$Radius
    )

    $diameter = $Radius * 2
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

$directory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $directory -Force | Out-Null

$bitmap = [System.Drawing.Bitmap]::new(64, 64, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.Clear([System.Drawing.Color]::Transparent)

$tile = New-RoundedRectangle -X 4 -Y 4 -Width 56 -Height 56 -Radius 12
$gradient = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
    [System.Drawing.RectangleF]::new(4, 4, 56, 56),
    [System.Drawing.Color]::FromArgb(255, 11, 24, 43),
    [System.Drawing.Color]::FromArgb(255, 6, 95, 70),
    35.0)
$graphics.FillPath($gradient, $tile)

$border = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 52, 211, 153), 3)
$graphics.DrawPath($border, $tile)

$vitaMark = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 226, 255, 247), 7)
$vitaMark.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$vitaMark.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$vitaMark.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
$graphics.DrawLines($vitaMark, [System.Drawing.PointF[]]@(
    [System.Drawing.PointF]::new(18, 20),
    [System.Drawing.PointF]::new(32, 46),
    [System.Drawing.PointF]::new(46, 20)
))

$accent = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 96, 165, 250))
$graphics.FillEllipse($accent, 48, 46, 7, 7)

$handle = $bitmap.GetHicon()
$icon = [System.Drawing.Icon]::FromHandle($handle)
$stream = [System.IO.File]::Open($OutputPath, [System.IO.FileMode]::Create)
try {
    $icon.Save($stream)
} finally {
    $stream.Dispose()
}

if ($PreviewPath) {
    $previewDirectory = Split-Path -Parent $PreviewPath
    New-Item -ItemType Directory -Path $previewDirectory -Force | Out-Null
    $bitmap.Save($PreviewPath, [System.Drawing.Imaging.ImageFormat]::Png)
}

$icon.Dispose()
[NativeMethods.IconHandle]::DestroyIcon($handle) | Out-Null
$accent.Dispose()
$vitaMark.Dispose()
$border.Dispose()
$gradient.Dispose()
$tile.Dispose()
$graphics.Dispose()
$bitmap.Dispose()
