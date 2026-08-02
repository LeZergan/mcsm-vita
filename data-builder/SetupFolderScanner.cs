namespace McsmVitaDataBuilder;

public sealed record SetupFolderScanResult(
    string? ApkPath,
    string? MainObbPath,
    string? PatchObbPath,
    IReadOnlyList<ChapterSource> Chapters)
{
    public bool BaseSetComplete => ApkPath is not null && MainObbPath is not null && PatchObbPath is not null;
}

public static class SetupFolderScanner
{
    public static SetupFolderScanResult Scan(string rootPath)
    {
        if (!Directory.Exists(rootPath))
        {
            throw new DirectoryNotFoundException("The selected setup folder does not exist.");
        }

        string root = Path.GetFullPath(rootPath);
        var options = new EnumerationOptions
        {
            RecurseSubdirectories = true,
            IgnoreInaccessible = true,
            AttributesToSkip = FileAttributes.ReparsePoint,
            ReturnSpecialDirectories = false
        };
        var files = Directory.EnumerateFiles(root, "*", options).ToList();

        string? apk = FindAccepted(
            files.Where(path => HasExtension(path, ".apk") && HasSize(path, DataBuilderService.SupportedApkBytes)),
            path => DataBuilderService.InspectApk(path));
        string? mainObb = FindAccepted(
            files.Where(path => HasExtension(path, ".obb")
                                && Path.GetFileName(path).StartsWith("main.", StringComparison.OrdinalIgnoreCase)
                                && HasSize(path, DataBuilderService.SupportedMainObbBytes)),
            path => DataBuilderService.InspectSupportedMainObbStandalone(path));
        string? patchObb = FindAccepted(
            files.Where(path => HasExtension(path, ".obb")
                                && Path.GetFileName(path).StartsWith("patch.", StringComparison.OrdinalIgnoreCase)
                                && HasSize(path, DataBuilderService.SupportedPatchObbBytes)),
            path => DataBuilderService.InspectPatchObb(path));

        IReadOnlyList<ChapterSource> chapters = FindChapters(files);
        return new SetupFolderScanResult(apk, mainObb, patchObb, chapters);
    }

    private static string? FindAccepted<T>(IEnumerable<string> candidates, Func<string, T> inspect)
    {
        foreach (string path in candidates.OrderBy(path => path.Length).ThenBy(path => path, StringComparer.OrdinalIgnoreCase))
        {
            try
            {
                _ = inspect(path);
                return path;
            }
            catch
            {
                // Wrong versions and renderer variants are expected in mixed folders.
            }
        }
        return null;
    }

    private static IReadOnlyList<ChapterSource> FindChapters(IReadOnlyList<string> files)
    {
        var candidates = new List<ChapterSource>();
        var seenPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        foreach (string marker in files.Where(IsEpisodeMarker))
        {
            string? parent = Path.GetDirectoryName(marker);
            if (parent is null || IsRuntimeOrBackupFolder(parent) || !seenPaths.Add(parent))
            {
                continue;
            }
            TryAddChapter(parent, candidates);
        }

        foreach (string package in files.Where(ChapterScanner.IsChapterPackage))
        {
            if (seenPaths.Add(package))
            {
                TryAddChapter(package, candidates);
            }
        }

        var selected = new List<ChapterSource>();
        var coveredEpisodes = new HashSet<int>();
        foreach (ChapterSource source in candidates
                     .OrderBy(source => source.Kind == ChapterSourceKind.Folder ? 0 : 1)
                     .ThenBy(source => source.Path.Length)
                     .ThenBy(source => source.Path, StringComparer.OrdinalIgnoreCase))
        {
            if (source.Episodes.All(coveredEpisodes.Contains))
            {
                continue;
            }
            selected.Add(source);
            foreach (int episode in source.Episodes)
            {
                coveredEpisodes.Add(episode);
            }
        }
        return selected;
    }

    private static void TryAddChapter(string path, ICollection<ChapterSource> chapters)
    {
        try
        {
            chapters.Add(ChapterScanner.Inspect(path));
        }
        catch
        {
            // A ZIP or folder with unrelated Lua/archive files is not a chapter source.
        }
    }

    private static bool IsEpisodeMarker(string path)
    {
        string fileName = Path.GetFileName(path);
        return fileName.StartsWith("MCSM_android_Minecraft10", StringComparison.OrdinalIgnoreCase)
            && fileName.EndsWith("_data.ttarch2", StringComparison.OrdinalIgnoreCase)
            && ChapterScanner.DetectEpisodes([fileName]).Count > 0;
    }

    private static bool IsRuntimeOrBackupFolder(string path)
    {
        string[] blocked = ["assets", "assets123", "diag", "glsl", "gxp", "Temp", "User"];
        string folderName = Path.GetFileName(path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        return blocked.Contains(folderName, StringComparer.OrdinalIgnoreCase);
    }

    private static bool HasExtension(string path, string extension) =>
        Path.GetExtension(path).Equals(extension, StringComparison.OrdinalIgnoreCase);

    private static bool HasSize(string path, long bytes)
    {
        try
        {
            return new FileInfo(path).Length == bytes;
        }
        catch
        {
            return false;
        }
    }
}
