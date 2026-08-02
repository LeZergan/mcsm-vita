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
    string OutputDirectory,
    IReadOnlyList<ChapterSource> ChapterSources,
    string GraphicsProfile,
    string LanguageCode);

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
    long TotalBytes);
