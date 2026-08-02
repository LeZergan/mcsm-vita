using System.IO.Compression;
using System.Text.RegularExpressions;

namespace McsmVitaDataBuilder;

public static partial class ChapterScanner
{
    private const int MaxArchiveDepth = 4;

    public static ChapterSource Inspect(string path)
    {
        string fullPath = Path.GetFullPath(path);
        var files = new List<(string Name, long Size)>();
        bool hasPowerVrEvidence = HasPowerVrMarker(fullPath);

        if (Directory.Exists(fullPath))
        {
            CollectDirectory(fullPath, files, ref hasPowerVrEvidence);
            return CreateSource(fullPath, ChapterSourceKind.Folder, files, hasPowerVrEvidence);
        }

        if (File.Exists(fullPath) && IsChapterPackage(fullPath))
        {
            CollectArchiveFile(fullPath, files, ref hasPowerVrEvidence, 0, requireArchive: true);
            ChapterSourceKind kind = Path.GetExtension(fullPath).Equals(".obb", StringComparison.OrdinalIgnoreCase)
                ? ChapterSourceKind.ObbArchive
                : ChapterSourceKind.ZipArchive;
            return CreateSource(fullPath, kind, files, hasPowerVrEvidence);
        }

        throw new InvalidDataException(
            "Choose an episode folder, chapter .obb, or .zip/full chapter bundle.");
    }

    public static bool IsChapterFile(string path)
    {
        string extension = Path.GetExtension(path);
        return (extension.Equals(".ttarch2", StringComparison.OrdinalIgnoreCase)
                || extension.Equals(".lua", StringComparison.OrdinalIgnoreCase))
            && !HasOtherRendererMarker(path);
    }

    public static bool IsChapterPackage(string path)
    {
        string extension = Path.GetExtension(path);
        return extension.Equals(".zip", StringComparison.OrdinalIgnoreCase)
            || extension.Equals(".obb", StringComparison.OrdinalIgnoreCase);
    }

    public static IReadOnlyList<int> DetectEpisodes(IEnumerable<string> fileNames)
    {
        var episodes = new SortedSet<int>();
        foreach (string fileName in fileNames)
        {
            Match match = EpisodeMarkerRegex().Match(Path.GetFileName(fileName));
            if (match.Success
                && int.TryParse(match.Groups[1].Value, out int episode)
                && episode is >= 2 and <= 8)
            {
                episodes.Add(episode);
            }
        }
        return episodes.ToList();
    }

    private static void CollectDirectory(
        string root,
        ICollection<(string Name, long Size)> files,
        ref bool hasPowerVrEvidence)
    {
        foreach (string file in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
        {
            hasPowerVrEvidence |= HasPowerVrMarker(file);
            if (IsChapterFile(file))
            {
                files.Add((Path.GetFileName(file), new FileInfo(file).Length));
                continue;
            }

            if (IsChapterPackage(file))
            {
                CollectArchiveFile(file, files, ref hasPowerVrEvidence, 0, requireArchive: false);
            }
        }
    }

    private static void CollectArchiveFile(
        string archivePath,
        ICollection<(string Name, long Size)> files,
        ref bool hasPowerVrEvidence,
        int depth,
        bool requireArchive)
    {
        if (depth > MaxArchiveDepth)
        {
            throw new InvalidDataException(
                $"Chapter packages may be nested no more than {MaxArchiveDepth} levels deep.");
        }

        ZipArchive archive;
        try
        {
            archive = ZipFile.OpenRead(archivePath);
        }
        catch (InvalidDataException) when (!requireArchive)
        {
            // A mixed setup folder can contain the base game's raw OBBs. They are not
            // ZIP-compatible chapter packages and are simply handled by the base picker.
            return;
        }
        using (archive)
        {
            CollectArchive(archive, archivePath, files, ref hasPowerVrEvidence, depth);
        }
    }

    private static void CollectArchive(
        ZipArchive archive,
        string sourceLabel,
        ICollection<(string Name, long Size)> files,
        ref bool hasPowerVrEvidence,
        int depth)
    {
        string? temporaryDirectory = null;
        try
        {
            int nestedIndex = 0;
            foreach (ZipArchiveEntry entry in archive.Entries.Where(entry => !string.IsNullOrEmpty(entry.Name)))
            {
                string logicalPath = $"{sourceLabel}!/{entry.FullName}";
                hasPowerVrEvidence |= HasPowerVrMarker(logicalPath);

                if (IsChapterFile(entry.FullName))
                {
                    files.Add((entry.Name, entry.Length));
                    continue;
                }

                if (!IsChapterPackage(entry.Name))
                {
                    continue;
                }
                if (depth >= MaxArchiveDepth)
                {
                    throw new InvalidDataException(
                        $"Chapter packages may be nested no more than {MaxArchiveDepth} levels deep.");
                }

                temporaryDirectory ??= CreateTemporaryDirectory();
                string nestedPath = Path.Combine(
                    temporaryDirectory,
                    $"{nestedIndex++:D4}{Path.GetExtension(entry.Name).ToLowerInvariant()}");
                using (Stream input = entry.Open())
                using (FileStream output = new(nestedPath, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    input.CopyTo(output);
                }

                CollectArchiveFile(
                    nestedPath,
                    files,
                    ref hasPowerVrEvidence,
                    depth + 1,
                    requireArchive: false);
            }
        }
        finally
        {
            DeleteTemporaryDirectory(temporaryDirectory);
        }
    }

    private static ChapterSource CreateSource(
        string path,
        ChapterSourceKind kind,
        IReadOnlyCollection<(string Name, long Size)> files,
        bool hasPowerVrEvidence)
    {
        if (files.Count == 0)
        {
            throw new InvalidDataException(
                "No compatible .ttarch2 or descriptor .lua files were found. " +
                "Mali and Adreno files are ignored.");
        }

        IReadOnlyList<int> episodes = DetectEpisodes(files.Select(file => file.Name));
        if (episodes.Count == 0)
        {
            throw new InvalidDataException(
                "Chapter files were found, but Minecraft10N_data.ttarch2 is missing. " +
                "Choose the complete episode folder, chapter OBB, or full chapter ZIP.");
        }
        if (!hasPowerVrEvidence)
        {
            throw new InvalidDataException(
                "The chapter files are not marked as PowerVR. Choose the PowerVR/SGX episode set; " +
                "Mali, Adreno, and unknown renderer packages are not accepted.");
        }

        return new ChapterSource(
            path,
            kind,
            episodes,
            files.Count,
            files.Sum(file => file.Size));
    }

    private static bool HasPowerVrMarker(string path) =>
        path.Contains("android-pvr", StringComparison.OrdinalIgnoreCase)
        || path.Contains("powervr", StringComparison.OrdinalIgnoreCase)
        || path.Contains("sgx", StringComparison.OrdinalIgnoreCase);

    private static bool HasOtherRendererMarker(string path) =>
        path.Contains("mali", StringComparison.OrdinalIgnoreCase)
        || path.Contains("adreno", StringComparison.OrdinalIgnoreCase);

    private static string CreateTemporaryDirectory()
    {
        string path = Path.Combine(Path.GetTempPath(), $"mcsm-chapter-scan-{Guid.NewGuid():N}");
        Directory.CreateDirectory(path);
        return path;
    }

    private static void DeleteTemporaryDirectory(string? path)
    {
        if (path is null || !Directory.Exists(path))
        {
            return;
        }
        string fullPath = Path.GetFullPath(path);
        string tempRoot = Path.GetFullPath(Path.GetTempPath());
        if (fullPath.StartsWith(tempRoot, StringComparison.OrdinalIgnoreCase)
            && Path.GetFileName(fullPath).StartsWith("mcsm-chapter-scan-", StringComparison.Ordinal))
        {
            Directory.Delete(fullPath, recursive: true);
        }
    }

    [GeneratedRegex(@"^MCSM_android_Minecraft10([2-8])_data\.ttarch2$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex EpisodeMarkerRegex();
}
