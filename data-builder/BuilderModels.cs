namespace McsmVitaDataBuilder;

public enum ChapterSourceKind
{
    Folder,
    ZipArchive
}

public sealed record ChapterSource(
    string Path,
    ChapterSourceKind Kind,
    IReadOnlyList<int> Episodes,
    int FileCount,
    long TotalBytes)
{
    public string DisplayName
    {
        get
        {
            string episodes = Episodes.Count == 1
                ? $"Episode {Episodes[0]}"
                : $"Episodes {string.Join(", ", Episodes)}";
            return $"{episodes}  ·  {FileCount} files  ·  {FormatBytes(TotalBytes)}";
        }
    }

    public override string ToString() => DisplayName;

    private static string FormatBytes(long bytes)
    {
        string[] units = ["B", "KB", "MB", "GB"];
        double value = bytes;
        int unit = 0;
        while (value >= 1024 && unit < units.Length - 1)
        {
            value /= 1024;
            unit++;
        }
        return $"{value:0.#} {units[unit]}";
    }
}

public sealed record BuildRequest(
    string ApkPath,
    string MainObbPath,
    string PatchObbPath,
    string OutputDirectory,
    IReadOnlyList<ChapterSource> ChapterSources,
    string GraphicsProfile,
    CustomProfileSettings? CustomProfile,
    string LanguageCode,
    string? ButtonFixPath,
    IReadOnlyList<DataAddonSource> DataAddons);

public sealed record CustomProfileSettings
{
    public string Mode { get; init; } = "easy";

    public string Picture { get; init; } = "sharp";
    public string Motion { get; init; } = "smooth";
    public string Gpu { get; init; } = "fastest";
    public string Effects { get; init; } = "outlines";
    public string World { get; init; } = "balanced";
    public string Power { get; init; } = "performance";

    public string Resolution { get; init; } = "720x408";
    public int FpsCap { get; init; } = 30;
    public string AdvancedGpu { get; init; } = "sgx540";
    public string Outlines { get; init; } = "on";
    public string Shadows { get; init; } = "off";
    public int Detail { get; init; } = 800;
    public int DrawDistance { get; init; } = 3500;
    public string Clock { get; init; } = "444";
    public string Upscale { get; init; } = "linear";
    public string Vsync { get; init; } = "on";
    public string NearestFilter { get; init; } = "off";
    public string FbfetchZero { get; init; } = "off";

    public string Summary => Mode == "advanced"
        ? $"Advanced · {Resolution} · {FpsCap} FPS · {AdvancedGpu.ToUpperInvariant()}"
        : $"Easy · {Cap(Picture)} · {MotionFps(Motion)} FPS · {GpuLabel(Gpu)}";

    private static string Cap(string value) =>
        string.IsNullOrEmpty(value) ? value : char.ToUpperInvariant(value[0]) + value[1..];

    private static int MotionFps(string motion) => motion switch
    {
        "low" => 15,
        "steady" => 20,
        _ => 30
    };

    private static string GpuLabel(string gpu) => gpu switch
    {
        "fast" => "SGX541",
        "medium" => "SGX542",
        "quality" => "SGX543",
        "original" => "SGX543MP",
        _ => "SGX540"
    };
}

public sealed record BuildProgress(
    int Percent,
    string Status,
    long BytesCopied,
    long TotalBytes);

public sealed record BuildResult(
    string OutputDirectory,
    string? BackupDirectory,
    IReadOnlyList<int> IncludedEpisodes,
    int ChapterFileCount,
    int ButtonFixFileCount,
    bool ChoiceDataIncluded,
    int DataAddonFileCount,
    int DataAddonOverwriteCount,
    long TotalBytes);

public sealed record BundledAsset(string Name, long Size);

public enum ButtonFixSourceKind
{
    Embedded,
    Folder,
    ZipArchive
}

public sealed record ButtonFixBundle(
    IReadOnlyList<BundledAsset> Assets,
    long TotalBytes,
    ButtonFixSourceKind Kind,
    string? Path)
{
    public int FileCount => Assets.Count;
}

public enum DataAddonSourceKind
{
    Folder,
    ZipArchive
}

public sealed record DataAddonSource(
    string Path,
    DataAddonSourceKind Kind,
    int FileCount,
    long TotalBytes)
{
    public string DisplayName =>
        $"{System.IO.Path.GetFileName(Path.TrimEnd(System.IO.Path.DirectorySeparatorChar))}  ·  " +
        $"{FileCount} files  ·  {FormatBytes(TotalBytes)}";

    public override string ToString() => DisplayName;

    private static string FormatBytes(long bytes)
    {
        string[] units = ["B", "KB", "MB", "GB"];
        double value = bytes;
        int unit = 0;
        while (value >= 1024 && unit < units.Length - 1)
        {
            value /= 1024;
            unit++;
        }
        return $"{value:0.#} {units[unit]}";
    }
}
